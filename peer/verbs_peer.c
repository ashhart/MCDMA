#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif
// Optional single-thread benchmark diagnostics, never part of provider state.
static int profile_latency;
static unsigned payload_bytes=4096;
static enum ibv_mtu path_mtu=IBV_MTU_1024;
static uint64_t last_post_ns;
static unsigned last_polls;
// Monotonic WR IDs: every submission gets a fresh identifier so a stale or
// duplicated completion from an earlier sample can never verify as current.
static uint64_t wr_sequence;
static uint64_t difference_ns(struct timespec end,struct timespec start) {
    return (uint64_t)(end.tv_sec-start.tv_sec)*1000000000ull+end.tv_nsec-start.tv_nsec;
}
static void fail(const char *where) { perror(where); exit(2); }
static void modify_or_fail(struct ibv_qp *qp,struct ibv_qp_attr *attr,int mask,const char *stage) {
    int result=ibv_modify_qp(qp,attr,mask);
    if (result) {
        // Verbs returns an error number; errno can still describe an older call.
        fprintf(stderr,"%s: modify_qp result=%d errno=%d (%s)\n",
                stage,result,errno,strerror(result>0?result:errno));
        exit(2);
    }
}
static int release_resources(struct ibv_context *ctx,struct ibv_pd *pd,
                              struct ibv_mr *mr,struct ibv_cq *cq,
                              struct ibv_qp *qp,unsigned char *memory) {
    int error=ibv_destroy_qp(qp);
    if (!error) error=ibv_destroy_cq(cq);
    if (!error) error=ibv_dereg_mr(mr);
    if (!error) { free(memory); error=ibv_dealloc_pd(pd); }
    // Retain pages after a failed hardware teardown; process exit lets the
    // kernel preserve any registration whose DMA ownership is unresolved.
    if (!error) error=ibv_close_device(ctx);
    return error;
}
static int post_and_wait(struct ibv_qp *qp,struct ibv_cq *cq,struct ibv_mr *mr,
                         unsigned char *buffer,uint64_t remote,uint32_t rkey,int is_read,uint64_t *elapsed_ns) {
    struct ibv_sge sge={.addr=(uintptr_t)buffer,.length=payload_bytes,.lkey=mr->lkey};
    struct ibv_send_wr wr={0},*bad=NULL;
    wr.wr_id=++wr_sequence; wr.sg_list=&sge; wr.num_sge=1;
    wr.opcode=is_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE; wr.send_flags=IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr=remote; wr.wr.rdma.rkey=rkey;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    struct timespec start,now,pause={.tv_sec=0,.tv_nsec=100000};
    if (clock_gettime(CLOCK_MONOTONIC_RAW,&start)) return 0;
    int error=ibv_post_send(qp,&wr,&bad);
    if (error) { fprintf(stderr,"post_send: %s\n",strerror(error)); return 0; }
    if (elapsed_ns && profile_latency) {
        if (clock_gettime(CLOCK_MONOTONIC_RAW,&now)) return 0;
        last_post_ns=difference_ns(now,start); last_polls=0;
    }
    for (;;) {
        if (elapsed_ns && profile_latency) ++last_polls;
        struct ibv_wc wc={0}; int n=ibv_poll_cq(cq,1,&wc);
        if (n<0) { fprintf(stderr,"poll_cq result=%d errno=%d\n",n,errno); return 0; }
        if (n==1) {
            if (wc.status!=IBV_WC_SUCCESS || wc.wr_id!=wr.wr_id ||
                wc.opcode!=(is_read ? IBV_WC_RDMA_READ : IBV_WC_RDMA_WRITE)) {
                fprintf(stderr,"CQ status=%u vendor=%u opcode=%u id=%llu\n",wc.status,wc.vendor_err,
                        wc.opcode,(unsigned long long)wc.wr_id);
                return 0;
            }
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (elapsed_ns) {
                if (clock_gettime(CLOCK_MONOTONIC_RAW,&now)) return 0;
                *elapsed_ns=difference_ns(now,start);
            }
            return 1;
        }
        if (clock_gettime(CLOCK_MONOTONIC_RAW,&now)) return 0;
        if (now.tv_sec-start.tv_sec>=5) {
            fprintf(stderr,"CQ timeout opcode=%u id=%llu\n",wr.opcode,(unsigned long long)wr.wr_id);
            return 0;
        }
        if (!elapsed_ns) nanosleep(&pause,NULL);
    }
}
static int compare_u64(const void *a,const void *b) {
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b; return (x>y)-(x<y);
}
// Sample-dependent payloads: a per-sample seed in the WRITE source and a
// poisoned READ destination make each timed verification distinguish a fresh
// transfer from bytes already present after an earlier successful READ.
static void fill_seeded(unsigned char *destination,uint64_t sample) {
    for (unsigned i=0;i<payload_bytes;++i) destination[i]=(unsigned char)(i*37+19+sample);
}
static int benchmark(struct ibv_qp *qp,struct ibv_cq *cq,struct ibv_mr *mr,
                     unsigned char *memory,uint64_t remote,uint32_t rkey) {
    enum { warmup=100, samples=1000 };
    char filename[]="/tmp/mcdma-cx5-latency-XXXXXX";
    int fd=mkstemp(filename); if (fd<0) return 0;
    FILE *trace=fdopen(fd,"w"); if (!trace) { close(fd); return 0; }
    fputs(profile_latency ? "operation,bytes,sample,completion_ns,post_ns,completion_wait_ns,poll_calls\n" :
          "operation,bytes,sample,completion_ns\n",trace);
    fprintf(stderr,"BENCHMARK_INTEGRITY monotonic_wr_ids=1 write_seed_per_sample=1 read_poison=0xa5\n");
    for (int read=0;read<=1;++read) {
        uint64_t ns[samples],post_ns[samples],elapsed=0,total=0;
        unsigned poll_calls[samples];
        for (unsigned i=0;i<warmup+samples;++i) {
            if (read) memset(memory+8192,0xa5,payload_bytes);
            else fill_seeded(memory+4096,i);
            if (!post_and_wait(qp,cq,mr,memory+(read ? 8192 : 4096),remote,rkey,read,&elapsed) ||
                (read && memcmp(memory+4096,memory+8192,payload_bytes))) { fclose(trace); return 0; }
            if (i>=warmup) {
                ns[i-warmup]=elapsed; total+=elapsed;
                if (profile_latency) {
                    post_ns[i-warmup]=last_post_ns; poll_calls[i-warmup]=last_polls;
                }
            }
        }
        for (unsigned i=0;i<samples;++i) {
            fprintf(trace,"%s,%u,%u,%llu",read ? "read" : "write",payload_bytes,i,(unsigned long long)ns[i]);
            if (profile_latency) fprintf(trace,",%llu,%llu,%u",(unsigned long long)post_ns[i],
                                        (unsigned long long)(ns[i]-post_ns[i]),poll_calls[i]);
            fputc('\n',trace);
        }
        qsort(ns,samples,sizeof(*ns),compare_u64);
        printf("LATENCY op=%s bytes=%u samples=%u warmup=%u qd=1 min_us=%.3f median_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f mean_us=%.3f\n",
               read ? "read" : "write",payload_bytes,samples,warmup,ns[0]/1000.0,(ns[499]+ns[500])/2000.0,
               ns[949]/1000.0,ns[989]/1000.0,ns[999]/1000.0,total/(1000.0*samples));
        fflush(stdout);
    }
    if (fclose(trace)) return 0;
    printf("LATENCY_TRACE %s\n",filename); fflush(stdout); return 1;
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    const char *bytes=getenv("MCDMA_PAYLOAD_BYTES");
    if (bytes && strcmp(bytes,"1024") && strcmp(bytes,"4096")) {
        fputs("Payload must be 1024 or 4096 bytes\n",stderr); return 2;
    }
    if (bytes) payload_bytes=(unsigned)strtoul(bytes,NULL,10);
    const char *mtu=getenv("MCDMA_PATH_MTU");
    if (mtu && strcmp(mtu,"1024") && strcmp(mtu,"4096")) {
        fputs("Path MTU must be 1024 or 4096 bytes\n",stderr); return 2;
    }
    if (mtu && !strcmp(mtu,"4096")) path_mtu=IBV_MTU_4096;
    fprintf(stderr,"BENCHMARK_CONFIG payload_bytes=%u path_mtu=%u\n",payload_bytes,128u<<path_mtu);
    profile_latency=getenv("MCDMA_LATENCY_PROFILE") && !strcmp(getenv("MCDMA_LATENCY_PROFILE"),"1");
#ifdef __APPLE__
    const char *qos=getenv("MCDMA_QOS");
    if (qos && strcmp(qos,"inherit")) {
        qos_class_t requested=!strcmp(qos,"interactive") ? QOS_CLASS_USER_INTERACTIVE :
                              !strcmp(qos,"initiated") ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UNSPECIFIED;
        if (requested==QOS_CLASS_UNSPECIFIED || pthread_set_qos_class_self_np(requested,0)) {
            fputs("Cannot set requested benchmark thread QoS\n",stderr); return 2;
        }
    }
    qos_class_t actual=QOS_CLASS_UNSPECIFIED; int relative=0;
    if (pthread_get_qos_class_np(pthread_self(),&actual,&relative)) return 2;
    fprintf(stderr,"BENCHMARK_THREAD qos_request=%s qos_actual=0x%x relative=%d profile=%d\n",
            qos?qos:"inherit",(unsigned)actual,relative,profile_latency);
#endif
    if (argc!=3 && (argc!=4 || (strcmp(argv[3],"readonly") && strcmp(argv[3],"initiator") && strcmp(argv[3],"responder") && strcmp(argv[3],"resources")))) {
        fputs("Usage: verbs-peer RDMA_DEVICE GID_INDEX [readonly|initiator|responder|resources]\n",stderr); return 2;
    }
    const int resources=argc==4 && !strcmp(argv[3],"resources");
    const int readonly=argc==4 && !strcmp(argv[3],"readonly");
    const int initiator=argc==4 && !strcmp(argv[3],"initiator");
    const int responder=argc==4 && !strcmp(argv[3],"responder");
    int count=0; struct ibv_device **list=ibv_get_device_list(&count); if (!list) fail("device list");
    struct ibv_context *ctx=NULL;
    for (int i=0;i<count;++i) if (!strcmp(ibv_get_device_name(list[i]),argv[1])) ctx=ibv_open_device(list[i]);
    ibv_free_device_list(list); if (!ctx) fail("open device");
    if (initiator || responder) {
        struct ibv_device_attr device={0};
        if (ibv_query_device(ctx,&device)) fail("query device");
        const int mellanox=device.vendor_id==0x15b3 && (device.vendor_part_id==0x1019 || device.vendor_part_id==0x1015);
        const int thunderbolt=device.vendor_id==0 && device.vendor_part_id==0; /* Apple TB-RDMA HCAs report zero ids */
        if (!mellanox && !thunderbolt) { fputs("Native initiator must be a ConnectX-4/5 or an Apple Thunderbolt RDMA device\n",stderr); return 2; }
    }
    struct ibv_port_attr port; if (ibv_query_port(ctx,1,&port)) fail("query port");
    /* Accept RoCE (Ethernet) and Apple Thunderbolt RDMA (link_layer 100) so the same client drives the Studio mesh. */
    if (port.state!=IBV_PORT_ACTIVE || (port.link_layer!=IBV_LINK_LAYER_ETHERNET && port.link_layer!=100)) { fputs("port is not active Ethernet/Thunderbolt\n",stderr); return 2; }
    if (path_mtu>port.active_mtu || path_mtu>port.max_mtu) {
        fprintf(stderr,"Requested path MTU exceeds port active/max MTU (%u/%u)\n",
                128u<<port.active_mtu,128u<<port.max_mtu); ibv_close_device(ctx); return 2;
    }
    int gid_index=atoi(argv[2]); union ibv_gid gid;
    if (ibv_query_gid(ctx,1,gid_index,&gid)) fail("query gid");
    struct ibv_pd *pd=ibv_alloc_pd(ctx); if (!pd) fail("PD");
    unsigned char *memory=NULL; if (posix_memalign((void **)&memory,16384,16384)) fail("memory");
    memset(memory,0,16384);
    struct ibv_mr *mr=ibv_reg_mr(pd,memory,16384,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|
                              (readonly ? 0 : IBV_ACCESS_REMOTE_WRITE));
    if (!mr) fail("MR");
    struct ibv_cq *cq=ibv_create_cq(ctx,31,NULL,NULL,0); if (!cq) fail("CQ");
    struct ibv_qp_init_attr init={0}; init.send_cq=cq; init.recv_cq=cq; init.qp_type=IBV_QPT_RC;
    init.cap.max_send_wr=31; init.cap.max_recv_wr=31; init.cap.max_send_sge=1; init.cap.max_recv_sge=1;
    struct ibv_qp *qp=ibv_create_qp(pd,&init); if (!qp) fail("QP");
    struct ibv_qp_attr attr={0}; attr.qp_state=IBV_QPS_INIT; attr.port_num=1;
    attr.qp_access_flags=IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE;
    modify_or_fail(qp,&attr,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS,"INIT");
    if (resources) {
        const int error=release_resources(ctx,pd,mr,cq,qp,memory);
        printf("NATIVE_RESOURCES bytes=16384 qp_init=1 cleanup=%d transfer_test=0\n",error);
        return error ? 2 : 0;
    }
    char gid_text[INET6_ADDRSTRLEN]; inet_ntop(AF_INET6,&gid,gid_text,sizeof(gid_text));
    printf("ENDPOINT %u %u %u %llu 16384 %s\n",qp->qp_num,0x654321u,mr->rkey,(unsigned long long)(uintptr_t)memory,gid_text); fflush(stdout);
    unsigned remote_qpn,remote_psn; char remote_gid[80];
    if (scanf("%u %u %79s",&remote_qpn,&remote_psn,remote_gid)!=3 || remote_qpn>0xffffff || remote_psn>0xffffff) return 2;
    memset(&attr,0,sizeof(attr)); attr.qp_state=IBV_QPS_RTR; attr.path_mtu=path_mtu;
    attr.dest_qp_num=remote_qpn; attr.rq_psn=remote_psn; attr.max_dest_rd_atomic=1; attr.min_rnr_timer=12;
    attr.ah_attr.is_global=1; attr.ah_attr.port_num=1; attr.ah_attr.grh.sgid_index=gid_index; attr.ah_attr.grh.hop_limit=64;
    if (inet_pton(AF_INET6,remote_gid,&attr.ah_attr.grh.dgid)!=1) return 2;
    modify_or_fail(qp,&attr,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER,"RTR");
    memset(&attr,0,sizeof(attr)); attr.qp_state=IBV_QPS_RTS; attr.timeout=14; attr.retry_cnt=7; attr.rnr_retry=7; attr.sq_psn=0x654321; attr.max_rd_atomic=1;
    modify_or_fail(qp,&attr,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC,"RTS");
    puts("READY"); fflush(stdout);
    char finish[16]; if (scanf("%15s",finish)!=1) return 2;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    int good=1;
    if (initiator && (!strcmp(finish,"INITIATE") || !strcmp(finish,"INITIATEBENCH"))) {
        const int measure=!strcmp(finish,"INITIATEBENCH");
        unsigned remote_key,remote_length; unsigned long long remote_address;
        if (scanf("%u %llu %u",&remote_key,&remote_address,&remote_length)!=3 ||
            !remote_key || remote_length<16384 || remote_address>UINT64_MAX-remote_length) return 2;
        for (unsigned i=0;i<payload_bytes;++i) memory[i]=(unsigned char)(i*37+19);
        memset(memory+8192,0xa5,payload_bytes);
        int wrote=post_and_wait(qp,cq,mr,memory,remote_address,remote_key,0,NULL);
        int read=wrote && post_and_wait(qp,cq,mr,memory+8192,remote_address,remote_key,1,NULL);
        good=read && !memcmp(memory,memory+8192,payload_bytes);
        printf("NATIVE_FORWARD write=%d read=%d verified=%u\n",wrote,read,good?payload_bytes:0); fflush(stdout);
        if (good && measure) {
            // Keep the forward verification pattern on the peer while timing
            // the same registered buffers; this copy is outside the measured loop.
            memcpy(memory+4096,memory,payload_bytes);
            if (!benchmark(qp,cq,mr,memory,remote_address,remote_key)) return 2;
            if (!post_and_wait(qp,cq,mr,memory,remote_address,remote_key,0,NULL)) return 2;
        }
        if (scanf("%15s",finish)!=1 || strcmp(finish,"CHECKREVERSE")) return 2;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        for (unsigned i=0;i<payload_bytes;++i) if (memory[4096+i]!=(unsigned char)(i*53+101)) good=0;
        printf("NATIVE_REVERSE verified=%u\n",good?payload_bytes:0);
    } else if (responder && !strcmp(finish,"CHECKREVERSE")) {
        for (unsigned i=0;i<payload_bytes;++i) if (memory[4096+i]!=(unsigned char)(i*53+101)) good=0;
        printf("NATIVE_REVERSE verified=%u\n",good?payload_bytes:0);
    } else if (readonly && !strcmp(finish,"UNCHANGED")) {
        for (unsigned i=0;i<16384;++i) if (memory[i]) good=0;
        printf("PEER_BUFFER_UNCHANGED=%u\n",good ? 16384 : 0);
    } else if (!readonly && (!strcmp(finish,"VERIFY") || !strcmp(finish,"ROUNDTRIP") || !strcmp(finish,"ROUNDTRIPBENCH") || !strcmp(finish,"REVERSE"))) {
        const int reverse_only=!strcmp(finish,"REVERSE");
        if (!reverse_only) for (unsigned i=0;i<payload_bytes;++i) if (memory[i]!=(unsigned char)(i*37+19)) good=0;
        if (!strcmp(finish,"VERIFY")) printf("PEER_WRITE_BYTES_VERIFIED=%u\n",good ? payload_bytes : 0);
        else {
            unsigned remote_key,remote_length; unsigned long long remote_address;
            if (scanf("%u %llu %u",&remote_key,&remote_address,&remote_length)!=3 || !remote_key ||
                remote_length<8192 || remote_address>UINT64_MAX-remote_length) return 2;
            for (unsigned i=0;i<payload_bytes;++i) memory[4096+i]=(unsigned char)(i*53+101);
            memset(memory+8192,0xa5,payload_bytes);
            // The Mac has no active control RPC during these two operations.
            int wrote=good && post_and_wait(qp,cq,mr,memory+4096,remote_address+4096,remote_key,0,NULL);
            int read=wrote && post_and_wait(qp,cq,mr,memory+8192,remote_address+4096,remote_key,1,NULL);
            int reverse=read && !memcmp(memory+4096,memory+8192,payload_bytes);
            printf("PEER_RESULT forward=%u write=%d read=%d reverse=%u\n",good && !reverse_only ? payload_bytes : 0,wrote,read,reverse ? payload_bytes : 0);
            good=good && reverse;
            fflush(stdout);
            if (good && !strcmp(finish,"ROUNDTRIPBENCH")) {
                good=benchmark(qp,cq,mr,memory,remote_address+4096,remote_key);
                // CHECKREVERSE expects this original pattern after the benchmark.
                for (unsigned i=0;i<payload_bytes;++i) memory[4096+i]=(unsigned char)(i*53+101);
                if (good) good=post_and_wait(qp,cq,mr,memory+4096,remote_address+4096,remote_key,0,NULL);
            }
        }
    } else return 2;
    fflush(stdout);
    int cleanup=release_resources(ctx,pd,mr,cq,qp,memory);
    return good && !cleanup ? 0 : 2;
}
