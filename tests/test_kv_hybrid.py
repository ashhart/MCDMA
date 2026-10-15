"""Offline tests of hybrid handoffs (Qwen3.8 Flash Next): canonical entries from fake vLLM caches. Needs torch."""
import pathlib
import sys
import unittest
from types import SimpleNamespace

try:
    import torch
except ImportError:  # the protocol tests in test_kv_handoff.py run without torch
    torch = None

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'integrations' / 'vllm'))

if torch is not None:
    from mcdma_kv import hybrid  # noqa: E402

TOKENS = 6
CONFIG = SimpleNamespace(linear_conv_kernel_dim=4, ple_conv_kernel_size=2, ngram_size=3, num_hidden_layers=4)
QSA = SimpleNamespace(num_kv_heads=2, head_size=4, block_size=4)


def values(*shape, start=0, dtype=None):
    count = 1
    for size in shape:
        count *= size
    return (torch.arange(count, dtype=torch.float32) + start).reshape(shape).to(dtype or torch.bfloat16)


def state_page(slots, slot, states):
    """A Mamba-type cache: per slot, the states' bytes back to back, as vLLM's bind_kv_cache lays them out."""
    raw = torch.cat([state.contiguous().reshape(-1).view(torch.uint8) for state in states])
    tensor = torch.zeros(slots, raw.numel(), dtype=torch.uint8)
    tensor[slot] = raw
    return tensor


def qsa_cache(blocks, block_ids, k, v):
    """[num_blocks, 2, block_size, heads * head_size], the request's rows on `block_ids` in order."""
    cache = torch.zeros(blocks, 2, QSA.block_size, QSA.num_kv_heads * QSA.head_size, dtype=torch.bfloat16)
    for row in range(k.shape[0]):
        block, offset = block_ids[row // QSA.block_size], row % QSA.block_size
        cache[block, 0, offset] = k[row].reshape(-1)
        cache[block, 1, offset] = v[row].reshape(-1)
    return cache


@unittest.skipIf(torch is None, 'torch is not installed')
class HybridExportTest(unittest.TestCase):
    def setUp(self):
        self.recorder = hybrid.IndexKeyRecorder(16)
        self.keys = {layer: values(TOKENS, 3, start=100 * layer) for layer in (3, 4)}
        for layer, keys in self.keys.items():
            module = SimpleNamespace(layer_name=f'{"mtp" if layer == 4 else "model"}.layers.{layer}.self_attn.attn')
            # Two prefill chunks, M-RoPE positions [3, rows] as the b12x QSA run passes them.
            for start, stop in ((0, 4), (4, TOKENS)):
                positions = torch.arange(start, stop).expand(3, -1)
                self.recorder.record(module, positions, keys[start:stop], stop - start)

    def caches(self, spec_tokens):
        conv = values(10, 3 + spec_tokens, start=1)          # vLLM's DS layout: [channels, window]
        ssm = values(2, 3, 2, dtype=torch.float32, start=500)
        ple = values(3 + spec_tokens, 7, start=900)          # the other layout: [window, channels]
        self.k = {layer: values(TOKENS, 2, 4, start=1000 * layer) for layer in (3, 4)}
        self.v = {layer: values(TOKENS, 2, 4, start=1000 * layer + 50) for layer in (3, 4)}
        self.conv, self.ssm, self.ple = conv, ssm, ple
        caches = {
            'model.layers.0.linear_attn': state_page(4, 2, [conv, ssm]),
            'model.layers.1.ple': state_page(4, 1, [ple]),
            'model.layers.3.self_attn.attn': qsa_cache(8, (5, 2), self.k[3], self.v[3]),
            'mtp.layers.4.self_attn.attn': qsa_cache(8, (6, 7), self.k[4], self.v[4]),
        }
        specs = [SimpleNamespace(shapes=[conv.shape, ssm.shape], dtypes=[torch.bfloat16, torch.float32]),
                 SimpleNamespace(shapes=[ple.shape], dtypes=[torch.bfloat16]), QSA, QSA]
        groups = {name: index for index, name in enumerate(caches)}
        block_ids = ((2,), (1,), (5, 2), (6, 7))
        return caches, groups, specs, block_ids

    def export(self, spec_tokens=0, hidden=None):
        caches, groups, specs, block_ids = self.caches(spec_tokens)
        entries = hybrid.export_entries(caches, groups, specs, block_ids, TOKENS, self.recorder, CONFIG,
                                        hidden, spec_tokens)
        return {(e.layer, e.kind, e.name): e for e in entries}

    def test_backbone_entries_take_the_canonical_layout(self):
        found = self.export()
        self.assertTrue(torch.equal(found[(3, 'qsa', 'k')].tensor, self.k[3]))
        self.assertTrue(torch.equal(found[(3, 'qsa', 'v')].tensor, self.v[3]))
        self.assertTrue(torch.equal(found[(3, 'qsa', 'index_keys')].tensor, self.keys[3]))
        self.assertTrue(torch.equal(found[(0, 'gdn', 'conv')].tensor, self.conv.transpose(0, 1)))
        self.assertTrue(torch.equal(found[(0, 'gdn', 'ssm')].tensor, self.ssm))
        self.assertTrue(torch.equal(found[(1, 'ple', 'conv')].tensor, self.ple))
        self.assertEqual(found[(0, 'gdn', 'ssm')].dtype, 'float32')
        self.assertEqual(found[(3, 'qsa', 'index_keys')].to_dict()['dims'], ['block', 'index_dim'])
        self.assertEqual(sorted(e.index for e in found.values()), list(range(len(found))))

    def test_without_a_hidden_state_the_mtp_head_is_left_out(self):
        self.assertFalse(any(kind == 'mtp' for _, kind, _ in self.export()))

    def test_speculative_windows_export_their_prefill_slots(self):
        found = self.export(spec_tokens=1)
        self.assertTrue(torch.equal(found[(0, 'gdn', 'conv')].tensor, self.conv.transpose(0, 1)[:3]))
        self.assertTrue(torch.equal(found[(1, 'ple', 'conv')].tensor, self.ple[:3]))
        with self.assertRaisesRegex(ValueError, 'window'):
            hybrid._channel_major(self.conv, 3, 2)

    def test_the_mtp_head_exports_prompt_pairs_and_the_last_hidden_state(self):
        hidden = hybrid.HiddenRecorder()
        rows = values(TOKENS, 20, start=7)
        hidden.record(torch.arange(4), rows[:4], 4)
        hidden.record(torch.arange(4, TOKENS + 2), torch.cat([rows[4:], torch.zeros(2, 20, dtype=rows.dtype)]),
                      TOKENS - 4)  # graph padding after the real rows is never read
        found = self.export(spec_tokens=1, hidden=hidden)
        self.assertTrue(torch.equal(found[(4, 'mtp', 'k')].tensor, self.k[4][:TOKENS - 1]))
        self.assertTrue(torch.equal(found[(4, 'mtp', 'v')].tensor, self.v[4][:TOKENS - 1]))
        self.assertTrue(torch.equal(found[(4, 'mtp', 'index_keys')].tensor, self.keys[4][:TOKENS - 1]))
        self.assertTrue(torch.equal(found[(4, 'mtp', 'hidden')].tensor, rows[TOKENS - 1:]))
        self.assertEqual(found[(4, 'mtp', 'hidden')].to_dict()['dims'], ['block', 'hidden'])
        self.assertNotIn((4, 'qsa', 'k'), found)

    def test_index_keys_must_have_been_recorded(self):
        with self.assertRaisesRegex(ValueError, 'layer 7'):
            self.recorder.keys(7, TOKENS)


if __name__ == '__main__':
    unittest.main()
