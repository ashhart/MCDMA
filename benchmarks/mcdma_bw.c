/* MCDMA sustained-bandwidth benchmark.
 *
 * One source for both ends of the link: on macOS it builds against Apple's
 * verbs headers and librdma (the MCDMA provider is loaded through
 * IBV_DRIVERS), on Linux against libibverbs. The two processes talk through
 * stdin/stdout: every "BW_MSG <payload>" line printed here is relayed by the
 * runner (benchmarks/run_bw.py) to the other process, which reads "<payload>"
 * from stdin. No addresses, hosts or keys are built in.
 *
 * Per trial the initiator posts ceil(total/bytes) work requests across the
 * queue pairs, at most --depth outstanding per QP, and then finishes with a
 * receiver-visible marker: a WRITE_WITH_IMM when the responder can receive
 * immediates and the initiator can post one, otherwise an 8-byte flag WRITE
 * into a known offset at the end of the responder's region, which the
 * responder polls. The data receiver verifies a seeded pattern on sampled
 * windows of every slot and a canary guard region after each trial.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { MAX_QPS=8, MAX_DEPTH=64, MIN_BYTES=4096, GUARD_BYTES=16384, FLAG_BYTES=64,
       WINDOW_BYTES=4096, LINE_BYTES=4096, POLL_BATCH=16 };
#define MAX_BYTES (16u<<20)
#define DEFAULT_TOTAL (64ull<<20)
#define DEFAULT_MAX_REGION (1ull<<30)
#define DEFAULT_VERIFY (4ull<<20)

enum op_kind { OP_WRITE, OP_READ, OP_SEND };
enum finish_mode { FINISH_AUTO, FINISH_FLAG, FINISH_IMM };
enum wr_kind { WR_DATA=1, WR_FINISH=2, WR_RECV=3, WR_PROBE=4 };

static const char *const op_names[]={"write","read","send"};

struct options {
    enum op_kind op; enum finish_mode finish;
    unsigned bytes, depth, qps, cq_per_qp, repeats, warmup, mtu_bytes, gid_index, timeout_s;
    uint64_t total, max_region, verify_bytes; unsigned psn;
    int initiator; const char *device;
    const char *payload_path, *dump_path; /* real-payload mode: fill slots from a file, dump landed bytes */
};

struct remote_info {
    uint64_t nonce, addr, length, total; unsigned depth, cqe, rd, imm_recv, psn, qps, bytes, mtu_bytes, cq_per_qp;
    uint32_t rkey, qpn[MAX_QPS]; char gid[64]; char op[8]; char role[16];
};

struct window { uint64_t offset, length; };

struct line_reader { char buf[LINE_BYTES]; size_t len; };

struct bench {
    struct options o;
    struct ibv_context *ctx; struct ibv_pd *pd; struct ibv_mr *mr;
    unsigned char *region; uint64_t region_bytes;
    struct ibv_cq *cq[MAX_QPS]; unsigned cqs, cq_cap[MAX_QPS], cq_inflight[MAX_QPS];
    struct ibv_qp *qp[MAX_QPS]; unsigned qp_wr; /* granted max_send_wr/max_recv_wr */
    unsigned depth_local, depth, depth_effective, cqe_requested, cqe_granted, rd_atomic, dest_rd;
    unsigned imm_recv, probe_recv_posted;
    uint64_t nonce, wr_seq;
    struct remote_info remote;
    int finish_imm; const char *finish_reason;
    struct line_reader in;
    struct window *windows; unsigned window_count;
    unsigned post_retries, trials_run, trials_failed;
    /* Per-trial bookkeeping. */
    uint64_t posted[MAX_QPS], completed[MAX_QPS], target[MAX_QPS], recv_posted[MAX_QPS], recv_done[MAX_QPS];
    unsigned inflight[MAX_QPS];
    char gid_text[INET6_ADDRSTRLEN];
};

/* ---- Small helpers ------------------------------------------------------ */
static double now_s(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (double)ts.tv_sec+(double)ts.tv_nsec/1e9;
}
static double cpu_s(void) {
    struct rusage ru; if (getrusage(RUSAGE_SELF,&ru)) return 0;
    return (double)ru.ru_utime.tv_sec+(double)ru.ru_utime.tv_usec/1e6+
           (double)ru.ru_stime.tv_sec+(double)ru.ru_stime.tv_usec/1e6;
}
static void fatal(const char *what,int error) {
    if (error) printf("BW_ERROR what=%s errno=%d text=%s\n",what,error,strerror(error));
    else printf("BW_ERROR what=%s\n",what);
    fflush(stdout); exit(2);
}
static unsigned next_smaller(unsigned n) { return n>3 ? (n+1)/2-1 : n-1; }
static uint64_t slot_seed(uint64_t nonce,unsigned trial,unsigned slot) {
    return nonce^((uint64_t)(trial+1)<<32)^((uint64_t)slot*0x9E3779B1ull)^0x4d43444d41ull;
}
static uint64_t pattern_word(uint64_t seed,uint64_t index) {
    uint64_t x=(seed+index)*0x9E3779B97F4A7C15ull; return x^(x>>31);
}
static void fill_words(unsigned char *p,uint64_t bytes,uint64_t seed,uint64_t first) {
    for (uint64_t i=0;i<bytes/8;++i) { uint64_t w=pattern_word(seed,first+i); memcpy(p+i*8,&w,8); }
}
static uint64_t verify_words(const unsigned char *p,uint64_t bytes,uint64_t seed,uint64_t first) {
    uint64_t bad=0;
    for (uint64_t i=0;i<bytes/8;++i) {
        uint64_t w=pattern_word(seed,first+i),have; memcpy(&have,p+i*8,8); if (have!=w) bad+=8;
    }
    return bad;
}
static uint64_t flag_value(uint64_t nonce,unsigned trial) {
    uint64_t v=pattern_word(nonce^0x464c4147ull,trial+1); return v ? v : 1;
}
static uint64_t parse_size(const char *text,int *ok) {
    char *end=NULL; errno=0; unsigned long long v=strtoull(text,&end,10);
    if (errno || end==text) { *ok=0; return 0; }
    if (!strcasecmp(end,"k")) v<<=10; else if (!strcasecmp(end,"m")) v<<=20; else if (!strcasecmp(end,"g")) v<<=30;
    else if (*end) { *ok=0; return 0; }
    *ok=1; return v;
}

/* ---- Line protocol ------------------------------------------------------ */
static void msg(const char *fmt,...) __attribute__((format(printf,1,2)));
static void msg(const char *fmt,...) {
    va_list ap; va_start(ap,fmt); fputs("BW_MSG ",stdout); vprintf(fmt,ap); va_end(ap); fputc('\n',stdout); fflush(stdout);
}
/* Returns 1 with a complete line in out, 0 on timeout, -1 on EOF/error. */
static int read_line(struct line_reader *r,char *out,size_t size,double timeout) {
    const double deadline=now_s()+timeout;
    for (;;) {
        char *nl=memchr(r->buf,'\n',r->len);
        if (nl) {
            size_t n=(size_t)(nl-r->buf);
            if (n>=size) fatal("protocol_line_too_long",0);
            memcpy(out,r->buf,n); out[n]=0;
            memmove(r->buf,nl+1,r->len-n-1); r->len-=n+1;
            return 1;
        }
        if (r->len>=sizeof(r->buf)) fatal("protocol_line_too_long",0);
        double remaining=deadline-now_s();
        if (remaining<0) remaining=0;
        fd_set set; FD_ZERO(&set); FD_SET(0,&set);
        struct timeval tv; tv.tv_sec=(time_t)remaining; tv.tv_usec=(suseconds_t)((remaining-(double)tv.tv_sec)*1e6);
        const int ready=select(1,&set,NULL,NULL,&tv);
        if (ready<0) { if (errno==EINTR) continue; return -1; }
        if (!ready) return 0;
        char chunk[256]; const size_t space=sizeof(r->buf)-r->len;
        const ssize_t got=read(0,chunk,space<sizeof(chunk) ? space : sizeof(chunk));
        if (got<=0) return -1;
        memcpy(r->buf+r->len,chunk,(size_t)got); r->len+=(size_t)got;
    }
}
/* Expects a line whose first word is `type`; anything else is a protocol error. */
static void expect_line(struct bench *b,const char *type,char *out,size_t size) {
    const int r=read_line(&b->in,out,size,b->o.timeout_s);
    if (r<=0) { printf("BW_ERROR what=protocol_wait_%s result=%d\n",type,r); fflush(stdout); exit(2); }
    const size_t n=strlen(type);
    if (strncmp(out,type,n) || (out[n] && out[n]!=' ')) { printf("BW_ERROR what=protocol_unexpected expected=%s got=%.200s\n",type,out); fflush(stdout); exit(2); }
}
/* Copies the value of key=value (bounded) into out; returns 0 if absent. */
static int field(const char *line,const char *key,char *out,size_t size) {
    const size_t n=strlen(key);
    for (const char *p=line;(p=strstr(p,key))!=NULL;p+=n) {
        if ((p==line || p[-1]==' ') && p[n]=='=') {
            const char *v=p+n+1; size_t len=strcspn(v," ");
            if (len>=size) return 0;
            memcpy(out,v,len); out[len]=0; return 1;
        }
    }
    return 0;
}
static uint64_t field_u64(const char *line,const char *key,uint64_t max) {
    char text[64]; if (!field(line,key,text,sizeof(text))) { printf("BW_ERROR what=protocol_missing_%s\n",key); fflush(stdout); exit(2); }
    char *end=NULL; errno=0; unsigned long long v=strtoull(text,&end,10);
    if (errno || end==text || *end || v>max) { printf("BW_ERROR what=protocol_bad_%s\n",key); fflush(stdout); exit(2); }
    return v;
}

/* ---- Setup -------------------------------------------------------------- */
static void usage(void) {
    fputs("usage: mcdma-bw --role initiator|responder --device NAME [--gid-index N]\n"
          "  --op write|read|send --bytes N[K|M] --depth D --qps Q [--cq-per-qp] --total N[K|M|G]\n"
          "  [--repeats R] [--warmup W] [--mtu 1024|2048|4096] [--finish auto|flag|imm]\n"
          "  [--verify-bytes N] [--max-region N] [--timeout SECONDS] [--psn N]\n",stderr);
    exit(2);
}
static void parse(struct bench *b,int argc,char **argv) {
    struct options *o=&b->o;
    o->op=OP_WRITE; o->finish=FINISH_AUTO; o->bytes=65536; o->depth=16; o->qps=1; o->repeats=3; o->warmup=1;
    o->mtu_bytes=1024; o->total=DEFAULT_TOTAL; o->max_region=DEFAULT_MAX_REGION; o->verify_bytes=DEFAULT_VERIFY;
    o->timeout_s=30; o->psn=0x654321; o->initiator=-1;
    for (int i=1;i<argc;++i) {
        const char *a=argv[i];
        if (!strcmp(a,"--cq-per-qp")) { o->cq_per_qp=1; continue; }
        if (!strcmp(a,"--help")) usage();
        if (i+1>=argc) usage();
        const char *v=argv[++i]; int ok=1;
        if (!strcmp(a,"--role")) { if (!strcmp(v,"initiator")) o->initiator=1; else if (!strcmp(v,"responder")) o->initiator=0; else usage(); }
        else if (!strcmp(a,"--device")) o->device=v;
        else if (!strcmp(a,"--gid-index")) o->gid_index=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--op")) { if (!strcmp(v,"write")) o->op=OP_WRITE; else if (!strcmp(v,"read")) o->op=OP_READ; else if (!strcmp(v,"send")) o->op=OP_SEND; else usage(); }
        else if (!strcmp(a,"--bytes")) o->bytes=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--depth")) o->depth=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--qps")) o->qps=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--total")) o->total=parse_size(v,&ok);
        else if (!strcmp(a,"--repeats")) o->repeats=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--warmup")) o->warmup=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--mtu")) o->mtu_bytes=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--finish")) { if (!strcmp(v,"auto")) o->finish=FINISH_AUTO; else if (!strcmp(v,"flag")) o->finish=FINISH_FLAG; else if (!strcmp(v,"imm")) o->finish=FINISH_IMM; else usage(); }
        else if (!strcmp(a,"--verify-bytes")) o->verify_bytes=parse_size(v,&ok);
        else if (!strcmp(a,"--max-region")) o->max_region=parse_size(v,&ok);
        else if (!strcmp(a,"--timeout")) o->timeout_s=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--psn")) o->psn=(unsigned)parse_size(v,&ok);
        else if (!strcmp(a,"--payload")) o->payload_path=v;
        else if (!strcmp(a,"--dump")) o->dump_path=v;
        else usage();
        if (!ok) usage();
    }
    if (o->initiator<0 || !o->device) usage();
    if (o->bytes<MIN_BYTES || o->bytes>MAX_BYTES || o->bytes%8) fatal("bytes_out_of_range_4KiB_16MiB",0);
    if (o->depth<1 || o->depth>MAX_DEPTH) fatal("depth_out_of_range_1_64",0);
    if (o->qps<1 || o->qps>MAX_QPS) fatal("qps_out_of_range_1_8",0);
    if (o->total<o->bytes || o->total>(1ull<<40)) fatal("total_must_cover_one_request",0);
    if (o->repeats<1 || o->repeats>10000 || o->warmup>10000) fatal("repeats_out_of_range",0);
    if (o->mtu_bytes!=1024 && o->mtu_bytes!=2048 && o->mtu_bytes!=4096) fatal("mtu_must_be_1024_2048_4096",0);
    if (o->timeout_s<1 || o->timeout_s>3600) fatal("timeout_out_of_range",0);
    if (o->psn>0xffffff || o->gid_index>255) fatal("psn_or_gid_index_out_of_range",0);
    if (o->max_region<(uint64_t)o->bytes+GUARD_BYTES) fatal("max_region_too_small_for_one_request",0);
}
static enum ibv_mtu mtu_enum(unsigned bytes) { return bytes==4096 ? IBV_MTU_4096 : bytes==2048 ? IBV_MTU_2048 : IBV_MTU_1024; }

static void open_device(struct bench *b) {
    int count=0; struct ibv_device **list=ibv_get_device_list(&count); if (!list) fatal("device_list",errno);
    for (int i=0;i<count;++i) if (!strcmp(ibv_get_device_name(list[i]),b->o.device)) b->ctx=ibv_open_device(list[i]);
    ibv_free_device_list(list);
    if (!b->ctx) fatal("open_device",errno);
    struct ibv_port_attr port; if (ibv_query_port(b->ctx,1,&port)) fatal("query_port",errno);
    if (port.state!=IBV_PORT_ACTIVE || port.link_layer!=IBV_LINK_LAYER_ETHERNET) fatal("port_not_active_ethernet",0);
    if (mtu_enum(b->o.mtu_bytes)>port.active_mtu || mtu_enum(b->o.mtu_bytes)>port.max_mtu) fatal("path_mtu_exceeds_port_mtu",0);
    union ibv_gid gid; if (ibv_query_gid(b->ctx,1,(int)b->o.gid_index,&gid)) fatal("query_gid",errno);
    inet_ntop(AF_INET6,gid.raw,b->gid_text,sizeof(b->gid_text));
    b->pd=ibv_alloc_pd(b->ctx); if (!b->pd) fatal("alloc_pd",errno);
}

static void create_queues(struct bench *b) {
    struct ibv_device_attr dev; memset(&dev,0,sizeof(dev));
    if (ibv_query_device(b->ctx,&dev)) fatal("query_device",errno);
    const struct options *o=&b->o;
    b->depth_local=o->depth;
    /* One region: slots for every outstanding request on every QP, plus a guard. */
    const uint64_t slot_bytes=(uint64_t)o->qps*o->bytes;
    if (slot_bytes*b->depth_local+GUARD_BYTES>o->max_region) {
        unsigned fit=(unsigned)((o->max_region-GUARD_BYTES)/slot_bytes);
        if (!fit) fatal("region_cap_below_one_slot_per_qp",0);
        printf("BW_CLAMP resource=region requested=%u granted=%u reason=max_region_%" PRIu64 "\n",b->depth_local,fit,o->max_region);
        b->depth_local=fit;
    }
    b->cqs=o->cq_per_qp ? o->qps : 1;
    unsigned want=(o->cq_per_qp ? b->depth_local : b->depth_local*o->qps)+2;
    b->cqe_requested=want;
    if (dev.max_cqe>0 && want>(unsigned)dev.max_cqe) {
        printf("BW_CLAMP resource=cq requested=%u granted=%d reason=device_max_cqe\n",want,dev.max_cqe);
        want=(unsigned)dev.max_cqe;
    }
    unsigned granted=want;
    for (unsigned c=0;c<b->cqs;++c) {
        for (;;) {
            b->cq[c]=ibv_create_cq(b->ctx,(int)granted,NULL,NULL,0);
            if (b->cq[c]) break;
            const int error=errno;
            if ((error!=EOPNOTSUPP && error!=EINVAL && error!=ENOMEM) || granted<=1) fatal("create_cq",error);
            granted=next_smaller(granted);
            printf("BW_CLAMP resource=cq requested=%u granted=%u reason=errno_%d\n",want,granted,error);
        }
        b->cq_cap[c]=b->cq[c]->cqe>0 && (unsigned)b->cq[c]->cqe<granted ? (unsigned)b->cq[c]->cqe : granted;
    }
    b->cqe_granted=granted;
    unsigned wr=b->depth_local;
    if (dev.max_qp_wr>0 && wr>(unsigned)dev.max_qp_wr) wr=(unsigned)dev.max_qp_wr;
    for (unsigned q=0;q<o->qps;++q) {
        for (;;) {
            struct ibv_qp_init_attr init; memset(&init,0,sizeof(init));
            init.send_cq=b->cq[o->cq_per_qp ? q : 0]; init.recv_cq=init.send_cq; init.qp_type=IBV_QPT_RC;
            init.cap.max_send_wr=wr; init.cap.max_recv_wr=wr; init.cap.max_send_sge=1; init.cap.max_recv_sge=1;
            b->qp[q]=ibv_create_qp(b->pd,&init);
            if (b->qp[q]) break;
            const int error=errno;
            if ((error!=EOPNOTSUPP && error!=EINVAL && error!=ENOMEM) || wr<=1) fatal("create_qp",error);
            const unsigned smaller=next_smaller(wr);
            printf("BW_CLAMP resource=qp_wr requested=%u granted=%u reason=errno_%d\n",wr,smaller,error);
            wr=smaller;
        }
    }
    b->qp_wr=wr;
    if (wr<b->depth_local) b->depth_local=wr;
    unsigned rd=b->depth_local>16 ? 16 : b->depth_local;
    if (dev.max_qp_rd_atom>0 && rd>(unsigned)dev.max_qp_rd_atom) rd=(unsigned)dev.max_qp_rd_atom;
    if (dev.max_qp_init_rd_atom>0 && rd>(unsigned)dev.max_qp_init_rd_atom) rd=(unsigned)dev.max_qp_init_rd_atom;
    b->rd_atomic=rd ? rd : 1;
    /* Register the region; a provider that caps registration size clamps depth. */
    for (;;) {
        b->region_bytes=slot_bytes*b->depth_local+GUARD_BYTES;
        void *memory=NULL;
        if (posix_memalign(&memory,16384,(size_t)b->region_bytes)) fatal("region_alloc",errno);
        memset(memory,0x5a,(size_t)b->region_bytes);
        b->region=memory;
        b->mr=ibv_reg_mr(b->pd,memory,(size_t)b->region_bytes,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ);
        if (b->mr) break;
        const int error=errno; free(memory); b->region=NULL;
        if (b->depth_local<=1) fatal("reg_mr",error);
        const unsigned smaller=next_smaller(b->depth_local);
        printf("BW_CLAMP resource=mr requested=%u granted=%u reason=errno_%d\n",b->depth_local,smaller,error);
        b->depth_local=smaller;
    }
    /* Outstanding requests per QP also bounded by this QP's share of its CQ,
     * with one entry kept free for the finish marker. */
    {
        const unsigned usable=b->cq_cap[0]>1 ? b->cq_cap[0]-1 : 1;
        const unsigned share=o->cq_per_qp ? usable : usable/o->qps;
        if (share && b->depth_local>share) {
            printf("BW_CLAMP resource=depth requested=%u granted=%u reason=cq_capacity_%u\n",b->depth_local,share,b->cqe_granted);
            b->depth_local=share;
        }
    }
    if (b->depth_local<o->depth)
        printf("BW_CLAMP resource=depth requested=%u granted=%u reason=local_limits\n",o->depth,b->depth_local);
    /* Immediate-data receives: the current MCDMA kernel path reports any
     * non-SEND receive format as a failed completion, so the Mac only offers
     * them when explicitly forced with --finish imm. */
#ifdef __APPLE__
    b->imm_recv=o->finish==FINISH_IMM;
#else
    b->imm_recv=o->finish!=FINISH_FLAG;
#endif
}

static void modify_or_fatal(struct ibv_qp *qp,struct ibv_qp_attr *attr,int mask,const char *stage) {
    const int r=ibv_modify_qp(qp,attr,mask);
    if (r) { printf("BW_ERROR what=modify_qp_%s result=%d text=%s\n",stage,r,strerror(r>0?r:errno)); fflush(stdout); exit(2); }
}
static int post_recv_slot(struct bench *b,unsigned q,unsigned char *address,uint32_t length,enum wr_kind kind) {
    struct ibv_sge sge; sge.addr=(uintptr_t)address; sge.length=length; sge.lkey=b->mr->lkey;
    struct ibv_recv_wr wr,*bad=NULL; memset(&wr,0,sizeof(wr));
    wr.wr_id=((uint64_t)q<<56)|((uint64_t)kind<<48)|(++b->wr_seq&0xffffffffffffull); wr.sg_list=&sge; wr.num_sge=1;
    const int r=ibv_post_recv(b->qp[q],&wr,&bad);
    if (!r) ++b->cq_inflight[b->o.cq_per_qp ? q : 0];
    return r;
}
static unsigned char *flag_address(struct bench *b) { return b->region+b->region_bytes-FLAG_BYTES; }
static uint64_t remote_flag(struct bench *b) { return b->remote.addr+b->remote.length-FLAG_BYTES; }

static void connect_queues(struct bench *b) {
    const struct options *o=&b->o;
    if (o->initiator) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
        b->nonce=((uint64_t)ts.tv_sec<<32)^(uint64_t)ts.tv_nsec^((uint64_t)getpid()<<48);
        if (!b->nonce) b->nonce=1;
    }
    char qpns[MAX_QPS*12]={0};
    for (unsigned q=0;q<o->qps;++q) { char one[12]; snprintf(one,sizeof(one),"%s%u",q?",":"",b->qp[q]->qp_num); strcat(qpns,one); }
    msg("ENDPOINT v=1 nonce=%" PRIu64 " role=%s op=%s bytes=%u depth=%u qps=%u cqpq=%u cqe=%u total=%" PRIu64
        " mtu=%u imm_recv=%u rd=%u psn=%u rkey=%u addr=%" PRIu64 " length=%" PRIu64 " gid=%s qpn=%s",
        b->nonce,o->initiator?"initiator":"responder",op_names[o->op],o->bytes,b->depth_local,o->qps,o->cq_per_qp,
        b->cqe_granted,o->total,o->mtu_bytes,b->imm_recv,b->rd_atomic,o->psn,b->mr->rkey,(uint64_t)(uintptr_t)b->region,
        b->region_bytes,b->gid_text,qpns);
    char line[LINE_BYTES]; expect_line(b,"ENDPOINT",line,sizeof(line));
    struct remote_info *r=&b->remote;
    if (field_u64(line,"v",UINT64_MAX)!=1) fatal("protocol_version",0);
    if (!field(line,"role",r->role,sizeof(r->role)) || !field(line,"op",r->op,sizeof(r->op)) || !field(line,"gid",r->gid,sizeof(r->gid))) fatal("protocol_endpoint_fields",0);
    if (!strcmp(r->role,o->initiator?"initiator":"responder") || strcmp(r->op,op_names[o->op])) fatal("protocol_role_or_op_mismatch",0);
    r->nonce=field_u64(line,"nonce",UINT64_MAX); r->bytes=(unsigned)field_u64(line,"bytes",MAX_BYTES);
    r->depth=(unsigned)field_u64(line,"depth",MAX_DEPTH); r->qps=(unsigned)field_u64(line,"qps",MAX_QPS);
    r->cq_per_qp=(unsigned)field_u64(line,"cqpq",1); r->cqe=(unsigned)field_u64(line,"cqe",1u<<20);
    r->total=field_u64(line,"total",1ull<<40); r->mtu_bytes=(unsigned)field_u64(line,"mtu",4096);
    r->imm_recv=(unsigned)field_u64(line,"imm_recv",1); r->rd=(unsigned)field_u64(line,"rd",255);
    r->psn=(unsigned)field_u64(line,"psn",0xffffff); r->rkey=(uint32_t)field_u64(line,"rkey",0xffffffff);
    r->addr=field_u64(line,"addr",UINT64_MAX); r->length=field_u64(line,"length",1ull<<40);
    if (r->bytes!=o->bytes || r->qps!=o->qps || r->total!=o->total || r->mtu_bytes!=o->mtu_bytes || r->cq_per_qp!=o->cq_per_qp)
        fatal("protocol_configuration_mismatch",0);
    if (!r->depth || !r->rkey || !r->length || r->addr>UINT64_MAX-r->length) fatal("protocol_endpoint_bounds",0);
    char text[MAX_QPS*12]; if (!field(line,"qpn",text,sizeof(text))) fatal("protocol_qpn",0);
    unsigned n=0; char *save=NULL;
    for (char *tok=strtok_r(text,",",&save);tok;tok=strtok_r(NULL,",",&save)) {
        char *end=NULL; unsigned long v=strtoul(tok,&end,10);
        if (*end || !v || v>0xffffff || n>=MAX_QPS) fatal("protocol_qpn",0);
        r->qpn[n++]=(uint32_t)v;
    }
    if (n!=o->qps) fatal("protocol_qpn_count",0);
    if (!o->initiator) { b->nonce=r->nonce; if (!b->nonce) fatal("protocol_nonce",0); }
    b->depth=b->depth_local<r->depth ? b->depth_local : r->depth;
    if (r->length<(uint64_t)o->qps*o->bytes*b->depth+GUARD_BYTES) fatal("protocol_remote_region_too_small",0);
    if (b->depth<o->depth) printf("BW_CLAMP resource=depth requested=%u granted=%u reason=agreed_with_peer\n",o->depth,b->depth);

    struct ibv_qp_attr attr;
    for (unsigned q=0;q<o->qps;++q) {
        memset(&attr,0,sizeof(attr)); attr.qp_state=IBV_QPS_INIT; attr.port_num=1;
        attr.qp_access_flags=IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE;
        modify_or_fatal(b->qp[q],&attr,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS,"init");
    }
    /* RTR: the responder-side read credit is offered first and confirmed to
     * the peer, so the initiator never exceeds what this side accepted. */
    unsigned dest=b->rd_atomic<r->rd ? b->rd_atomic : r->rd;
    for (unsigned q=0;q<o->qps;++q) {
        for (;;) {
            memset(&attr,0,sizeof(attr)); attr.qp_state=IBV_QPS_RTR; attr.path_mtu=mtu_enum(o->mtu_bytes);
            attr.dest_qp_num=r->qpn[q]; attr.rq_psn=r->psn; attr.max_dest_rd_atomic=(uint8_t)dest; attr.min_rnr_timer=12;
            attr.ah_attr.is_global=1; attr.ah_attr.port_num=1; attr.ah_attr.grh.sgid_index=(uint8_t)o->gid_index; attr.ah_attr.grh.hop_limit=64;
            if (inet_pton(AF_INET6,r->gid,&attr.ah_attr.grh.dgid)!=1) fatal("protocol_gid",0);
            const int rc=ibv_modify_qp(b->qp[q],&attr,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER);
            if (!rc) break;
            if (dest<=1 || q) { printf("BW_ERROR what=modify_qp_rtr result=%d text=%s\n",rc,strerror(rc>0?rc:errno)); fflush(stdout); exit(2); }
            printf("BW_CLAMP resource=dest_rd_atomic requested=%u granted=1 reason=result_%d\n",dest,rc); dest=1;
        }
    }
    b->dest_rd=dest;
    msg("RTR dest_rd=%u",dest);
    expect_line(b,"RTR",line,sizeof(line));
    const unsigned peer_dest=(unsigned)field_u64(line,"dest_rd",255);
    unsigned rd=b->rd_atomic<peer_dest ? b->rd_atomic : peer_dest; if (!rd) rd=1;
    for (unsigned q=0;q<o->qps;++q) {
        for (;;) {
            memset(&attr,0,sizeof(attr)); attr.qp_state=IBV_QPS_RTS; attr.timeout=14; attr.retry_cnt=7; attr.rnr_retry=7;
            attr.sq_psn=o->psn; attr.max_rd_atomic=(uint8_t)rd;
            const int rc=ibv_modify_qp(b->qp[q],&attr,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC);
            if (!rc) break;
            if (rd<=1 || q) { printf("BW_ERROR what=modify_qp_rts result=%d text=%s\n",rc,strerror(rc>0?rc:errno)); fflush(stdout); exit(2); }
            printf("BW_CLAMP resource=rd_atomic requested=%u granted=1 reason=result_%d\n",rd,rc); rd=1;
        }
    }
    b->rd_atomic=rd;
    /* A responder that offers immediates keeps one receive posted for the probe. */
    if (!o->initiator && b->imm_recv) {
        if (post_recv_slot(b,0,b->region,o->bytes,WR_PROBE)) fatal("post_recv_probe",errno);
        b->probe_recv_posted=1;
    }
    msg("READY rd=%u dest_rd=%u",rd,dest);
    expect_line(b,"READY",line,sizeof(line));
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/* ---- Completion handling ------------------------------------------------ */
struct poll_stats { uint64_t completions, errors, imm_seen, finish_seen, probe_seen, recv_bytes; uint32_t imm_value; };

static void handle_wc(struct bench *b,unsigned c,const struct ibv_wc *wc,struct poll_stats *s) {
    const unsigned q=(unsigned)(wc->wr_id>>56), kind=(unsigned)((wc->wr_id>>48)&0xff);
    if (b->cq_inflight[c]) --b->cq_inflight[c];
    if (wc->status!=IBV_WC_SUCCESS) {
        ++s->errors;
        fprintf(stderr,"BW_WC status=%u vendor=%u opcode=%u qp=%u kind=%u\n",wc->status,wc->vendor_err,wc->opcode,q,kind);
        if (kind==WR_DATA && q<MAX_QPS && b->inflight[q]) { --b->inflight[q]; ++b->completed[q]; }
        return;
    }
    if (kind==WR_DATA) {
        const enum ibv_wc_opcode expected=b->o.op==OP_READ ? IBV_WC_RDMA_READ : b->o.op==OP_WRITE ? IBV_WC_RDMA_WRITE : IBV_WC_SEND;
        if (wc->opcode!=expected) { ++s->errors; fprintf(stderr,"BW_WC unexpected_opcode=%u expected=%u\n",wc->opcode,expected); }
        if (q<MAX_QPS && b->inflight[q]) --b->inflight[q];
        if (q<MAX_QPS) ++b->completed[q];
        ++s->completions;
    } else if (kind==WR_FINISH || kind==WR_PROBE) {
        if (wc->opcode&IBV_WC_RECV) {
            /* Immediate carried by a remote WRITE: the trial marker itself. */
            if (!(wc->wc_flags&IBV_WC_WITH_IMM)) { ++s->errors; return; }
            s->imm_value=ntohl(wc->imm_data); ++s->imm_seen;
            if (kind==WR_PROBE) ++s->probe_seen; else ++s->finish_seen;
        } else { if (kind==WR_PROBE) ++s->probe_seen; else ++s->finish_seen; }
    } else if (kind==WR_RECV) {
        if (wc->wc_flags&IBV_WC_WITH_IMM) { s->imm_value=ntohl(wc->imm_data); ++s->imm_seen; ++s->finish_seen; return; }
        if (!(wc->opcode&IBV_WC_RECV) || wc->byte_len!=b->o.bytes) { ++s->errors; fprintf(stderr,"BW_WC recv opcode=%u bytes=%u\n",wc->opcode,wc->byte_len); }
        if (q<MAX_QPS) ++b->recv_done[q];
        s->recv_bytes+=wc->byte_len; ++s->completions;
    }
}
static int poll_all(struct bench *b,struct poll_stats *s) {
    int total=0;
    for (unsigned c=0;c<b->cqs;++c) {
        struct ibv_wc wcs[POLL_BATCH];
        const int n=ibv_poll_cq(b->cq[c],POLL_BATCH,wcs);
        if (n<0) { ++s->errors; fprintf(stderr,"BW_POLL cq=%u result=%d errno=%d\n",c,n,errno); return -1; }
        for (int i=0;i<n;++i) handle_wc(b,c,&wcs[i],s);
        total+=n;
    }
    return total;
}
static unsigned cq_of(struct bench *b,unsigned q) { return b->o.cq_per_qp ? q : 0; }
static unsigned cq_room(struct bench *b,unsigned c) {
    /* Keep one entry free for the finish marker and never exceed the CQ. */
    const unsigned cap=b->cq_cap[c]>1 ? b->cq_cap[c]-1 : 1;
    return b->cq_inflight[c]<cap ? cap-b->cq_inflight[c] : 0;
}
static unsigned char *slot_address(struct bench *b,unsigned q,uint64_t k) {
    return b->region+((uint64_t)q*b->depth+(k%b->depth))*b->o.bytes;
}
static uint64_t remote_slot(struct bench *b,unsigned q,uint64_t k) {
    return b->remote.addr+((uint64_t)q*b->depth+(k%b->depth))*b->o.bytes;
}
static int post_data(struct bench *b,unsigned q,uint64_t k) {
    struct ibv_sge sge; sge.addr=(uintptr_t)slot_address(b,q,k); sge.length=b->o.bytes; sge.lkey=b->mr->lkey;
    struct ibv_send_wr wr,*bad=NULL; memset(&wr,0,sizeof(wr));
    wr.wr_id=((uint64_t)q<<56)|((uint64_t)WR_DATA<<48)|(++b->wr_seq&0xffffffffffffull);
    wr.sg_list=&sge; wr.num_sge=1; wr.send_flags=IBV_SEND_SIGNALED;
    wr.opcode=b->o.op==OP_READ ? IBV_WR_RDMA_READ : b->o.op==OP_WRITE ? IBV_WR_RDMA_WRITE : IBV_WR_SEND;
    if (b->o.op!=OP_SEND) { wr.wr.rdma.remote_addr=remote_slot(b,q,k); wr.wr.rdma.rkey=b->remote.rkey; }
    return ibv_post_send(b->qp[q],&wr,&bad);
}
static int post_marker(struct bench *b,int imm,uint32_t imm_value,enum wr_kind kind) {
    unsigned char *source=flag_address(b);
    struct ibv_sge sge; sge.addr=(uintptr_t)source; sge.length=8; sge.lkey=b->mr->lkey;
    struct ibv_send_wr wr,*bad=NULL; memset(&wr,0,sizeof(wr));
    wr.wr_id=((uint64_t)0<<56)|((uint64_t)kind<<48)|(++b->wr_seq&0xffffffffffffull);
    wr.sg_list=&sge; wr.num_sge=1; wr.send_flags=IBV_SEND_SIGNALED;
    wr.opcode=imm ? IBV_WR_RDMA_WRITE_WITH_IMM : IBV_WR_RDMA_WRITE;
    if (imm) wr.imm_data=htonl(imm_value);
    wr.wr.rdma.remote_addr=remote_flag(b); wr.wr.rdma.rkey=b->remote.rkey;
    return ibv_post_send(b->qp[0],&wr,&bad);
}

/* ---- Verification windows --------------------------------------------- */
static void build_windows(struct bench *b) {
    const unsigned slots=b->o.qps*b->depth;
    b->windows=calloc((size_t)slots*3,sizeof(*b->windows)); if (!b->windows) fatal("windows_alloc",0);
    uint64_t budget=b->o.verify_bytes; unsigned n=0;
    for (unsigned s=0;s<slots && budget;++s) {
        const uint64_t base=(uint64_t)s*b->o.bytes;
        if (b->o.bytes<=3*WINDOW_BYTES) {
            uint64_t len=b->o.bytes<budget ? b->o.bytes : budget&~7ull; if (!len) break;
            b->windows[n].offset=base; b->windows[n].length=len; ++n; budget-=len; continue;
        }
        const uint64_t starts[3]={0,(b->o.bytes/2)&~7u,b->o.bytes-WINDOW_BYTES};
        for (unsigned w=0;w<3 && budget;++w) {
            uint64_t len=WINDOW_BYTES<budget ? WINDOW_BYTES : budget&~7ull; if (!len) break;
            b->windows[n].offset=base+starts[w]; b->windows[n].length=len; ++n; budget-=len;
        }
    }
    b->window_count=n;
}
static void poison_windows(struct bench *b) {
    for (unsigned i=0;i<b->window_count;++i) memset(b->region+b->windows[i].offset,0xa5,(size_t)b->windows[i].length);
}
static unsigned char *g_payload; static uint64_t g_payload_len;
static void load_payload(struct bench *b) {
    if (!b->o.payload_path) return;
    FILE *f=fopen(b->o.payload_path,"rb"); if (!f) fatal("payload_open",errno);
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if (n<=0) fatal("payload_empty",0);
    g_payload=malloc((size_t)n); if (!g_payload) fatal("payload_alloc",0);
    if (fread(g_payload,1,(size_t)n,f)!=(size_t)n) fatal("payload_read",errno);
    fclose(f); g_payload_len=(uint64_t)n;
    msg("PAYLOAD path=%s bytes=%" PRIu64,b->o.payload_path,g_payload_len);
}
static void fill_slots(struct bench *b,unsigned trial) {
    const unsigned slots=b->o.qps*b->depth;
    if (g_payload) { /* slot s carries payload chunk s (mod file), so a whole trial streams the file in order */
        for (unsigned s=0;s<slots;++s) { uint64_t off=((uint64_t)s*b->o.bytes)%g_payload_len; unsigned char *dst=b->region+(uint64_t)s*b->o.bytes;
            for (uint64_t done=0;done<b->o.bytes;) { uint64_t take=g_payload_len-off; if (take>b->o.bytes-done) take=b->o.bytes-done; memcpy(dst+done,g_payload+off,(size_t)take); done+=take; off=(off+take)%g_payload_len; } }
        return; }
    for (unsigned s=0;s<slots;++s) fill_words(b->region+(uint64_t)s*b->o.bytes,b->o.bytes,slot_seed(b->nonce,trial,s),0);
}
static void dump_landed(struct bench *b) {
    if (!b->o.dump_path) return;
    const uint64_t used=(uint64_t)b->o.qps*b->depth*b->o.bytes;
    FILE *f=fopen(b->o.dump_path,"wb"); if (!f) fatal("dump_open",errno);
    if (fwrite(b->region,1,(size_t)used,f)!=(size_t)used) fatal("dump_write",errno);
    fclose(f);
    msg("DUMP path=%s bytes=%" PRIu64,b->o.dump_path,used);
}
static uint64_t verify_windows(struct bench *b,unsigned trial,uint64_t *verified) {
    uint64_t bad=0; *verified=0;
    for (unsigned i=0;i<b->window_count;++i) {
        const uint64_t offset=b->windows[i].offset, slot=offset/b->o.bytes, inner=offset%b->o.bytes;
        bad+=verify_words(b->region+offset,b->windows[i].length,slot_seed(b->nonce,trial,(unsigned)slot),inner/8);
        *verified+=b->windows[i].length;
    }
    return bad;
}
static int guard_intact(struct bench *b) {
    const uint64_t used=(uint64_t)b->o.qps*b->depth*b->o.bytes, end=b->region_bytes-FLAG_BYTES;
    uint64_t first_end=used+GUARD_BYTES<end ? used+GUARD_BYTES : end;
    for (uint64_t i=used;i<first_end;++i) if (b->region[i]!=0x5a) return 0;
    for (uint64_t i=end-used>GUARD_BYTES ? end-GUARD_BYTES : used;i<end;++i) if (b->region[i]!=0x5a) return 0;
    return 1;
}

/* ---- Finish negotiation ------------------------------------------------- */
static void negotiate_finish(struct bench *b) {
    char line[LINE_BYTES];
    if (b->o.initiator) {
        if (b->o.finish==FINISH_FLAG) { b->finish_imm=0; b->finish_reason="requested_flag"; }
        else if (!b->remote.imm_recv) { b->finish_imm=0; b->finish_reason="responder_no_imm_receive"; }
        else {
            const int rc=post_marker(b,1,0x50524f42u,WR_PROBE);
            if (rc) { b->finish_imm=0; char text[64]; snprintf(text,sizeof(text),"post_imm_errno_%d",rc); b->finish_reason=strdup(text); }
            else {
                ++b->cq_inflight[0];
                struct poll_stats s; memset(&s,0,sizeof(s)); const double deadline=now_s()+b->o.timeout_s;
                while (!s.probe_seen && !s.errors) { if (poll_all(b,&s)<0 || now_s()>deadline) fatal("probe_imm_completion",0); }
                if (s.errors) fatal("probe_imm_failed_completion",0);
                b->finish_imm=1; b->finish_reason="probe_imm_completed";
            }
        }
        if (!b->finish_imm && b->o.finish==FINISH_IMM) fatal("finish_imm_unavailable",0);
        msg("FINISH method=%s reason=%s",b->finish_imm?"imm":"flag",b->finish_reason);
    } else {
        expect_line(b,"FINISH",line,sizeof(line));
        char method[8]; if (!field(line,"method",method,sizeof(method))) fatal("protocol_finish",0);
        b->finish_imm=!strcmp(method,"imm"); b->finish_reason="peer_decision";
        if (b->finish_imm) {
            if (!b->probe_recv_posted) fatal("protocol_finish_imm_without_receive",0);
            struct poll_stats s; memset(&s,0,sizeof(s)); const double deadline=now_s()+b->o.timeout_s;
            while (!s.probe_seen && !s.errors) { if (poll_all(b,&s)<0 || now_s()>deadline) fatal("probe_imm_receive",0); }
            if (s.errors || s.imm_value!=0x50524f42u) fatal("probe_imm_receive_mismatch",0);
            b->probe_recv_posted=0;
        }
    }
    printf("BW_FINISH method=%s reason=%s\n",b->finish_imm?"imm":"flag",b->finish_reason); fflush(stdout);
}

/* ---- Trials ------------------------------------------------------------- */
struct trial_result {
    unsigned trial, warmup; uint64_t total_bytes, wrs, completions, errors, verified, mismatches;
    double seconds, cpu_pct, responder_seconds, responder_cpu_pct; unsigned guard_ok, post_retries;
};
static void print_result(struct bench *b,const struct trial_result *t,const char *side) {
    static int header_done;
    const double gbit=t->seconds>0 ? (double)t->total_bytes*8/t->seconds/1e9 : 0;
    printf("BW_RESULT side=%s trial=%u warmup=%u op=%s bytes=%u depth=%u depth_effective=%u qps=%u cq_per_qp=%u cqe=%u mtu=%u"
           " total_bytes=%" PRIu64 " wrs=%" PRIu64 " seconds=%.6f gbit=%.4f completions=%" PRIu64 " errors=%" PRIu64 " cpu_pct=%.1f"
           " responder_seconds=%.6f responder_cpu_pct=%.1f verified_bytes=%" PRIu64 " mismatches=%" PRIu64 " finish=%s rd_atomic=%u"
           " post_retries=%u guard_ok=%u\n",
           side,t->trial,t->warmup,op_names[b->o.op],b->o.bytes,b->o.depth,b->depth_effective,b->o.qps,b->o.cq_per_qp,b->cqe_granted,
           b->o.mtu_bytes,t->total_bytes,t->wrs,t->seconds,gbit,t->completions,t->errors,t->cpu_pct,t->responder_seconds,
           t->responder_cpu_pct,t->verified,t->mismatches,b->finish_imm?"imm":"flag",b->rd_atomic,t->post_retries,t->guard_ok);
    if (!header_done) {
        puts("BW_CSV_HEADER side,trial,warmup,op,bytes,depth,depth_effective,qps,cq_per_qp,cqe,mtu,total_bytes,wrs,seconds,gbit,"
             "completions,errors,cpu_pct,responder_seconds,responder_cpu_pct,verified_bytes,mismatches,finish,rd_atomic,post_retries,guard_ok");
        header_done=1;
    }
    printf("BW_CSV %s,%u,%u,%s,%u,%u,%u,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%.6f,%.4f,%" PRIu64 ",%" PRIu64 ",%.1f,%.6f,%.1f,%" PRIu64
           ",%" PRIu64 ",%s,%u,%u,%u\n",
           side,t->trial,t->warmup,op_names[b->o.op],b->o.bytes,b->o.depth,b->depth_effective,b->o.qps,b->o.cq_per_qp,b->cqe_granted,
           b->o.mtu_bytes,t->total_bytes,t->wrs,t->seconds,gbit,t->completions,t->errors,t->cpu_pct,t->responder_seconds,
           t->responder_cpu_pct,t->verified,t->mismatches,b->finish_imm?"imm":"flag",b->rd_atomic,t->post_retries,t->guard_ok);
    fflush(stdout);
}
static void plan_trial(struct bench *b,uint64_t *wrs) {
    const uint64_t n=(b->o.total+b->o.bytes-1)/b->o.bytes;
    for (unsigned q=0;q<b->o.qps;++q) {
        b->target[q]=n/b->o.qps+(q<n%b->o.qps ? 1 : 0);
        b->posted[q]=b->completed[q]=b->recv_posted[q]=b->recv_done[q]=0; b->inflight[q]=0;
    }
    *wrs=n;
}

static int run_initiator_trial(struct bench *b,unsigned trial,unsigned warmup) {
    struct trial_result t; memset(&t,0,sizeof(t)); t.trial=trial; t.warmup=warmup;
    plan_trial(b,&t.wrs); t.total_bytes=t.wrs*b->o.bytes;
    if (b->o.op==OP_READ) poison_windows(b); else fill_slots(b,trial);
    const uint64_t flag=flag_value(b->nonce,trial); memcpy(flag_address(b),&flag,8);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    char line[LINE_BYTES]; expect_line(b,"ARMED",line,sizeof(line));
    if (field_u64(line,"trial",UINT32_MAX)!=trial) fatal("protocol_armed_trial",0);
    struct poll_stats s; memset(&s,0,sizeof(s));
    const unsigned retries_before=b->post_retries;
    const double start=now_s(), cpu0=cpu_s(), deadline=start+b->o.timeout_s;
    uint64_t done=0; int failed=0;
    while (done<t.wrs && !failed) {
        for (unsigned q=0;q<b->o.qps;++q) {
            const unsigned c=cq_of(b,q);
            while (b->posted[q]<b->target[q] && b->inflight[q]<b->depth_effective && cq_room(b,c)) {
                const int rc=post_data(b,q,b->posted[q]);
                if (rc==ENOMEM || rc==EAGAIN) { ++b->post_retries; break; }
                if (rc) { ++s.errors; fprintf(stderr,"BW_POST qp=%u errno=%d\n",q,rc); failed=1; break; }
                ++b->posted[q]; ++b->inflight[q]; ++b->cq_inflight[c];
            }
        }
        if (poll_all(b,&s)<0) { failed=1; break; }
        done=0; for (unsigned q=0;q<b->o.qps;++q) done+=b->completed[q];
        if (now_s()>deadline) { fprintf(stderr,"BW_TIMEOUT phase=data done=%" PRIu64 " of=%" PRIu64 "\n",done,t.wrs); failed=1; }
    }
    if (!failed && b->o.op!=OP_READ) {
        const int rc=post_marker(b,b->finish_imm,(uint32_t)(flag&0xffffffffu),WR_FINISH);
        if (rc) { ++s.errors; fprintf(stderr,"BW_POST finish errno=%d\n",rc); failed=1; }
        else {
            ++b->cq_inflight[0];
            while (!s.finish_seen && !failed) {
                if (poll_all(b,&s)<0) failed=1;
                if (now_s()>deadline) { fputs("BW_TIMEOUT phase=finish\n",stderr); failed=1; }
            }
        }
    }
    t.seconds=now_s()-start; t.cpu_pct=t.seconds>0 ? (cpu_s()-cpu0)/t.seconds*100 : 0;
    t.completions=s.completions; t.errors=s.errors+(failed?1:0); t.post_retries=b->post_retries-retries_before;
    if (b->o.op==OP_READ) { __atomic_thread_fence(__ATOMIC_SEQ_CST); t.mismatches=verify_windows(b,trial,&t.verified); }
    t.guard_ok=guard_intact(b);
    msg("COMPLETE trial=%u ok=%u",trial,!failed);
    expect_line(b,"DONE",line,sizeof(line));
    if (field_u64(line,"trial",UINT32_MAX)!=trial) fatal("protocol_done_trial",0);
    char text[64];
    if (field(line,"seconds",text,sizeof(text))) t.responder_seconds=atof(text);
    if (field(line,"cpu_pct",text,sizeof(text))) t.responder_cpu_pct=atof(text);
    if (b->o.op!=OP_READ) { t.verified=field_u64(line,"verified",UINT64_MAX); t.mismatches=field_u64(line,"mismatches",UINT64_MAX); }
    t.errors+=field_u64(line,"errors",UINT64_MAX);
    if (!field_u64(line,"guard_ok",1)) t.guard_ok=0;
    /* Drain any late completions so the next trial starts with an empty CQ. */
    for (unsigned c=0;c<b->cqs;++c) { struct ibv_wc wcs[POLL_BATCH]; int n; while ((n=ibv_poll_cq(b->cq[c],POLL_BATCH,wcs))>0) for (int i=0;i<n;++i) handle_wc(b,c,&wcs[i],&s); }
    print_result(b,&t,"initiator");
    return !failed && !t.errors && !t.mismatches && t.guard_ok;
}

static int keep_receives_posted(struct bench *b,struct poll_stats *s) {
    for (unsigned q=0;q<b->o.qps;++q) {
        const unsigned c=cq_of(b,q);
        while (b->recv_posted[q]<b->target[q] && b->recv_posted[q]-b->recv_done[q]<b->depth_effective && cq_room(b,c)) {
            const int rc=post_recv_slot(b,q,slot_address(b,q,b->recv_posted[q]),b->o.bytes,WR_RECV);
            if (rc==ENOMEM || rc==EAGAIN) { ++b->post_retries; break; }
            if (rc) { ++s->errors; fprintf(stderr,"BW_POST recv qp=%u errno=%d\n",q,rc); return 0; }
            ++b->recv_posted[q];
        }
    }
    return 1;
}
static int run_responder_trial(struct bench *b,unsigned trial,unsigned warmup) {
    struct trial_result t; memset(&t,0,sizeof(t)); t.trial=trial; t.warmup=warmup;
    plan_trial(b,&t.wrs); t.total_bytes=t.wrs*b->o.bytes;
    if (b->o.op==OP_READ) fill_slots(b,trial); else poison_windows(b);
    volatile uint64_t *flag=(volatile uint64_t *)(void *)flag_address(b); *flag=0;
    const uint64_t expected=flag_value(b->nonce,trial);
    struct poll_stats s; memset(&s,0,sizeof(s)); int failed=0, finish_recv_posted=0;
    if (b->o.op==OP_SEND) {
        /* A probe receive left unconsumed already covers QP 0's first slot. */
        if (b->probe_recv_posted) { b->recv_posted[0]=1; b->probe_recv_posted=0; }
        if (!keep_receives_posted(b,&s)) failed=1;
    } else if (b->finish_imm) {
        if (post_recv_slot(b,0,flag_address(b),FLAG_BYTES,WR_FINISH)) { ++s.errors; failed=1; }
        finish_recv_posted=1;
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    msg("ARMED trial=%u",trial);
    const unsigned retries_before=b->post_retries;
    const double start=now_s(), cpu0=cpu_s(), deadline=start+b->o.timeout_s;
    char line[LINE_BYTES]; int complete=0, have_complete=0; unsigned spins=0;
    while (!failed) {
        if (b->o.op==OP_SEND) {
            if (!keep_receives_posted(b,&s)) { failed=1; break; }
            /* The immediate finish consumes one receive after QP 0's last data
             * receive; post it as soon as that one is in the queue. */
            const unsigned c=cq_of(b,0);
            if (b->finish_imm && !finish_recv_posted && b->recv_posted[0]==b->target[0] && b->cq_inflight[c]<b->cq_cap[c]) {
                if (post_recv_slot(b,0,flag_address(b),FLAG_BYTES,WR_FINISH)) { ++s.errors; failed=1; break; }
                finish_recv_posted=1;
            }
        }
        if (b->o.op==OP_READ) {
            /* Nothing to observe locally: the initiator's completion ends the trial. */
            const int r=read_line(&b->in,line,sizeof(line),0.001);
            if (r<0) fatal("protocol_eof",0);
            if (r>0) { if (strncmp(line,"COMPLETE ",9)) fatal("protocol_unexpected_in_trial",0); have_complete=complete=1; }
        } else {
            if (poll_all(b,&s)<0) { failed=1; break; }
            if (b->finish_imm ? s.finish_seen : *flag==expected) {
                uint64_t data_done=0; for (unsigned q=0;q<b->o.qps;++q) data_done+=b->recv_done[q];
                if (b->o.op!=OP_SEND || data_done==t.wrs) complete=1;
            }
            if (!complete && !(++spins&255u)) {
                /* An initiator that gave up sends COMPLETE without a marker. */
                const int r=read_line(&b->in,line,sizeof(line),0);
                if (r<0) fatal("protocol_eof",0);
                if (r>0) { if (strncmp(line,"COMPLETE ",9)) fatal("protocol_unexpected_in_trial",0); have_complete=1; ++s.errors; failed=1; }
            }
        }
        if (complete) break;
        if (now_s()>deadline) { fputs("BW_TIMEOUT phase=responder\n",stderr); failed=1; }
    }
    t.seconds=now_s()-start; t.cpu_pct=t.seconds>0 ? (cpu_s()-cpu0)/t.seconds*100 : 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (b->finish_imm && s.finish_seen && s.imm_value!=(uint32_t)(expected&0xffffffffu)) { ++s.errors; fputs("BW_WC imm_mismatch\n",stderr); }
    if (b->o.op!=OP_READ) { if (b->o.dump_path) { if (!warmup) dump_landed(b); t.verified=0; t.mismatches=0; } else t.mismatches=verify_windows(b,trial,&t.verified); }
    if (!have_complete) expect_line(b,"COMPLETE",line,sizeof(line));
    if (field_u64(line,"trial",UINT32_MAX)!=trial) fatal("protocol_complete_trial",0);
    if (!field_u64(line,"ok",1)) ++s.errors;
    t.completions=s.completions; t.errors=s.errors+(failed?1:0); t.post_retries=b->post_retries-retries_before;
    t.guard_ok=guard_intact(b); t.responder_seconds=t.seconds; t.responder_cpu_pct=t.cpu_pct;
    msg("DONE trial=%u seconds=%.6f cpu_pct=%.1f verified=%" PRIu64 " mismatches=%" PRIu64 " errors=%" PRIu64 " completions=%" PRIu64 " guard_ok=%u",
        trial,t.seconds,t.cpu_pct,t.verified,t.mismatches,t.errors,t.completions,t.guard_ok);
    print_result(b,&t,"responder");
    return !failed && !t.errors && !t.mismatches && t.guard_ok;
}

static int release(struct bench *b) {
    int error=0;
    for (unsigned q=0;q<b->o.qps;++q) if (b->qp[q] && !error) error=ibv_destroy_qp(b->qp[q]);
    for (unsigned c=0;c<b->cqs;++c) if (b->cq[c] && !error) error=ibv_destroy_cq(b->cq[c]);
    if (!error && b->mr) error=ibv_dereg_mr(b->mr);
    /* Keep the pages if hardware teardown failed: DMA ownership is unresolved. */
    if (!error) { free(b->region); error=ibv_dealloc_pd(b->pd); }
    if (!error) error=ibv_close_device(b->ctx);
    return error;
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    static struct bench b;
    parse(&b,argc,argv);
    load_payload(&b);
    printf("BW_CONFIG role=%s op=%s bytes=%u depth=%u qps=%u cq_per_qp=%u total=%" PRIu64 " repeats=%u warmup=%u mtu=%u finish=%s"
           " verify_bytes=%" PRIu64 " max_region=%" PRIu64 " platform=%s\n",
           b.o.initiator?"initiator":"responder",op_names[b.o.op],b.o.bytes,b.o.depth,b.o.qps,b.o.cq_per_qp,b.o.total,b.o.repeats,
           b.o.warmup,b.o.mtu_bytes,b.o.finish==FINISH_AUTO?"auto":b.o.finish==FINISH_FLAG?"flag":"imm",b.o.verify_bytes,b.o.max_region,
#ifdef __APPLE__
           "darwin"
#else
           "linux"
#endif
           );
    open_device(&b);
    create_queues(&b);
    connect_queues(&b);
    build_windows(&b);
    b.depth_effective=b.depth; /* Agreed with the peer; both ends advertised their own bound. */
    negotiate_finish(&b);
    printf("BW_SETUP depth=%u depth_effective=%u cqs=%u cqe=%u qp_wr=%u rd_atomic=%u dest_rd=%u region_bytes=%" PRIu64 " windows=%u finish=%s\n",
           b.depth,b.depth_effective,b.cqs,b.cqe_granted,b.qp_wr,b.rd_atomic,b.dest_rd,b.region_bytes,b.window_count,b.finish_imm?"imm":"flag");
    fflush(stdout);
    const unsigned trials=b.o.warmup+b.o.repeats;
    for (unsigned trial=0;trial<trials;++trial) {
        const unsigned warmup=trial<b.o.warmup;
        const int ok=b.o.initiator ? run_initiator_trial(&b,trial,warmup) : run_responder_trial(&b,trial,warmup);
        ++b.trials_run; if (!ok) ++b.trials_failed;
        if (!ok) break; /* A failed trial can leave work in flight; later trials would mislabel it. */
    }
    const int cleanup=release(&b);
    printf("BW_DONE trials=%u failed=%u cleanup=%d\n",b.trials_run,b.trials_failed,cleanup);
    fflush(stdout);
    return b.trials_failed || cleanup ? 2 : 0;
}
