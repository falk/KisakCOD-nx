"""Synthetic profile-format invariants; no game assets or captures."""
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("profile_format", Path(__file__).resolve().parents[2] / "tools" / "perf" / "profile_format.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FormatTests(unittest.TestCase):
    def setUp(self):
        self.parser = module.ProfileParser()

    def native(self, **changes):
        value = dict(v=1, type="metrics", stream="cpu", seq=1, clock="monotonic_us", time=120,
                     unit="us", aggregation="window_mean", metrics={"future_counter": 23}, tags={"mode": "test"})
        value.update(changes)
        return self.parser.parse_line("KPERF " + json.dumps(value), 1)

    def test_unknown_metrics_tags(self):
        row = self.native(future_field={"ignored": True})[0]
        self.assertEqual(row["metrics"], {"future_counter": 23})
        self.assertEqual(row["tags"], {"mode": "test"})
        self.assertNotIn("id", row)

    def test_single_native_sample(self):
        row = self.native(type="sample", frame=9)[0]
        self.assertEqual(row["type"], "sample")
        self.assertEqual(row["aggregation"], "sample")
        self.assertEqual(row["seq"], 1)
        self.assertEqual(row["frame"], 9)

    def test_batch_sequence_once(self):
        rows = self.native(type="samples", metric="gpu", fields=["frame", "time", "value", "width", "height", "list_seq"],
                           samples=[[1, 100, 12.3, 960, 544, 4], [2, 120, 14.2, 864, 488, 6]], unit="ms")
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[1]["frame"], 2)
        self.assertEqual(rows[1]["metrics"], {"gpu": 14.2})
        self.assertEqual(rows[1]["tags"]["width"], 864)
        self.assertEqual(rows[1]["aggregation"], "sample")
        self.assertFalse(self.parser.issues)
        self.assertEqual(len(self.native(seq=2)), 1)

    def test_sequence_gap_and_stream_independence(self):
        self.native()
        self.native(stream="gpu", seq=80)
        rows = self.native(seq=3)
        self.assertEqual(rows[0]["name"], "sequence_discontinuity")
        self.assertEqual(self.parser.issues["sequence_discontinuity"]["count"], 1)

    def test_validation(self):
        for changes in ({"v": 2}, {"unit": 12}, {"metrics": {"x": float("nan")}}, {"time": -1},
                        {"type": "samples", "metric": "gpu", "fields": ["frame", "time", "value"], "samples": [[1, 2]]},
                        {"tags": ["bad"]}, {"seq": True}):
            with self.subTest(changes=changes):
                self.assertEqual(self.native(**changes)[0]["type"], "event")
        for text in ('KPERF {', 'KPERF {"v":1,"time":Infinity}', 'KPERF ' + 'x' * 65536):
            self.assertEqual(self.parser.parse_line(text, 1)[0]["type"], "event")

    def test_prefixes_units_legacy_windows(self):
        rows = self.parser.parse_line("[ 1.2] PERF fps=60.0 frame=16.7ms min=15ms max=28ms", 9)
        self.assertEqual(rows[0]["unit"], "fps")
        self.assertEqual(rows[1]["metrics"]["max"], 28)
        self.assertEqual(rows[1]["clock"], "source_line")
        self.assertEqual(rows[1]["aggregation"], "window_mean")
        self.assertNotIn("frame", rows[1])
        row = self.parser.parse_line("01:02:03.456 |W| thread KernelSvc OutputDebugString: SWITCH_PERF backend rbframe=4000 swapwait=20", 10)[0]
        self.assertEqual(row["unit"], "us")
        self.assertEqual(row["metrics"]["rbframe"], 4000)
        native = json.dumps(dict(v=1, type="metrics", stream="gpu", time=100, metrics={"x": 1}))
        self.assertEqual(self.parser.parse_line("DEKO9 KPERF " + native, 11)[0]["origin"], "native")

    def test_worker_stack_is_memory(self):
        rows = self.parser.parse_line("SWITCH_PERF worker w0=100 sv=200 svframes=3 svframe=60 svstack=16384", 1)
        stack = next(r for r in rows if "svstack" in r["metrics"])
        self.assertEqual(stack["unit"], "bytes")
        self.assertEqual(stack["aggregation"], "gauge")
        self.assertTrue(all("svstack" not in r["metrics"] for r in rows if r["unit"] == "us"))

    def test_gpu_cpu_loss_clocks(self):
        rows = self.parser.parse_line("DEKO9 perf frames=60 period=16.7ms gpu=12.3ms drawCpu=2.1ms fenceWait=0.1ms acquire=0.2ms draws=2000 lists=2 frameWaits=1 frameWait=0.3ms render=960x544", 1)
        self.assertEqual(rows[0]["unit"], "ms")
        self.assertEqual(rows[1]["unit"], "count/frame")
        self.assertEqual(rows[0]["tags"]["latest_render_size"], "960x544")
        self.assertEqual(self.parser.parse_line("DEKO9 gpupass frames=60 total=12.0 lit=10.2 dropped=3", 2)[0]["name"], "gpu_pass_loss")
        self.parser.parse_line("LOG_DROPPED 5", 3)
        self.parser.parse_line("SWITCH_CLOCKS unavailable rc=0x1", 4)
        self.assertEqual(self.parser.issues["transport_loss"]["count"], 5)
        self.assertEqual(self.parser.metadata["clock_source_line"], 4)
        self.assertIn("unavailable", self.parser.metadata["clocks"])
        self.assertEqual(self.parser.parse_line("SWITCH_PERFCONFIG request=0x1 rc=0x0", 5)[0]["name"], "configuration")
        audio = self.parser.parse_line("SWITCH_PERF sndmix backend=audren busy_us=1200 window_us=1000000 busy_pct=0.12 calls=60 us_per_call=20", 5)
        self.assertEqual({r["unit"] for r in audio}, {"us", "percent", "count"})

    def test_worker_command_lines(self):
        rows = self.parser.parse_line("SWITCH_PERF wrkcmd idle.m=500 idle.w=0 m.fxspot=2000 w.fxspot=0 m.dpvsent=0 w.dpvsent=4000", 1)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["unit"], "us")
        self.assertEqual(rows[0]["stream"], "legacy.cpu.wrkcmd")
        self.assertEqual(rows[0]["metrics"]["w.dpvsent"], 4000)
        self.assertEqual(rows[0]["metrics"]["idle.m"], 500)
        rows = self.parser.parse_line("SWITCH_PERF wrkwait front=700 all=0 fxspot=0 dpvsent=4000", 2)
        self.assertEqual(rows[0]["unit"], "us")
        self.assertEqual(rows[0]["metrics"]["front"], 700)
        rows = self.parser.parse_line("SWITCH_PERF wrkcount idle.m=1.0 idle.w=0.0 wait.front=1.0 wait.all=0.0 m.fxspot=1.0 w.fxspot=0.0 wait.fxspot=0.0", 3)
        self.assertEqual(rows[0]["unit"], "count/frame")
        self.assertEqual(rows[0]["metrics"]["wait.front"], 1.0)
        rows = self.parser.parse_line("SWITCH_PERF slowframe frame=145 trigger=wall wall=2000 idle.m=3000 wrk.m=700 resid=5000 "
                                      "scene.total=9000 frame.eventloop=2500 x.gpu=12000 x.lockwait=1500", 6)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["unit"], "us")
        self.assertEqual(rows[0]["aggregation"], "frame_delta")
        self.assertEqual(rows[0]["tags"]["frame_id"], 145)
        self.assertEqual(rows[0]["tags"]["trigger"], "wall")
        self.assertEqual(rows[0]["metrics"]["x.lockwait"], 1500)
        self.assertNotIn("frame", rows[0]["metrics"])
        # The scene residual can be negative when flat counters overlap.
        rows = self.parser.parse_line("SWITCH_PERF scene total=10 setup=3 misc=0 resid=-2", 4)
        self.assertEqual(rows[0]["metrics"]["resid"], -2)
        rows = self.parser.parse_line("DEKO9 perf frames=60 period=16.7ms gpu=12.3ms draws=2000 render=960x544 resizes=1 moves=2 "
                                      "spHits=3 spMisses=2 translates=2 translateUs=900 compiles=2 compileUs=7000 compileMaxUs=6000 "
                                      "bakes=4 bakeUs=8000 bakeMaxUs=6500 bakesUnlocked=0 firstBinds=5 firstBindBakeUs=7900 "
                                      "firstBindBakeMaxUs=6500 slowCompiles=1 slowUnlogged=0", 5)
        extra = next(r for r in rows if r["stream"] == "legacy.renderer.extra")
        self.assertEqual(extra["metrics"]["compileMaxUs"], 6000)
        self.assertEqual(extra["metrics"]["firstBindBakeUs"], 7900)
        self.assertEqual(rows[0]["tags"]["latest_render_size"], "960x544")

    def test_audio_units_and_aggregation(self):
        rows = self.parser.parse_line("SWITCH_PERF sndstream active=2 starts=1 open_us=50 load_us=300 load_max_us=100 slowest=test fills=20 fill_us=800 fill_max_us=200 max_tick_gap_ms=14 min_queued_ms=18 underruns=1 al_errors=0", 7)
        fields = {key: (row["unit"], row["aggregation"]) for row in rows for key in row["metrics"]}
        self.assertEqual(fields["fill_us"], ("us", "window_total"))
        self.assertEqual(fields["load_max_us"], ("us", "window_max"))
        self.assertEqual(fields["min_queued_ms"], ("ms", "window_min"))
        self.assertEqual(fields["active"], ("count", "gauge"))
        rows = self.parser.parse_line("SWITCH_PERF sndloop frames=60 calls=20.1 oor=1.0 cont=5.0 start_ok=1.2 start_fail=0 oneshot=2 cont_us=30 start_us=40 last_fail=test type=3 ent=5 entchan=2", 8)
        fields = {key: (row["unit"], row["aggregation"]) for row in rows for key in row["metrics"]}
        self.assertEqual(fields["calls"], ("count/frame", "mean_per_frame"))
        self.assertEqual(fields["cont_us"], ("us", "mean_per_frame"))
        self.assertEqual(fields["ent"], ("mixed_unknown", "unknown"))

    def test_native_events_are_data(self):
        message = '<script>alert("x")</script>'
        row = self.native(type="event", name="loss", count=4, message=message)[0]
        self.assertEqual(row["message"], message)
        self.assertEqual(row["count"], 4)
        self.assertEqual(self.parser.parse_line("CRASH:GPU_QUEUE_ERROR <img onerror=x>", 2)[0]["name"], "crash")

    def test_export_roundtrip(self):
        rows = self.native(type="samples", metric="wall", fields=["frame", "time", "value"], samples=[[1, 100, 12], [2, 120, 14]], unit="us")
        importer = module.ProfileParser()
        for i, row in enumerate(rows):
            exported = dict(row, id="discard-me")
            imported = importer.parse_line(json.dumps(exported), i + 1)[0]
            self.assertEqual(imported, row)
            self.assertNotIn("id", imported)
        self.assertFalse(importer.issues)
        legacy = self.parser.parse_line("PERF fps=60 frame=16.7ms min=14ms max=30ms", 8)
        for row in legacy:
            self.assertEqual(importer.parse_line(json.dumps(row), 20)[0], row)
        event = self.parser.parse_line("LOG_DROPPED 4", 9)[0]
        self.assertEqual(importer.parse_line(json.dumps(event), 21)[0], event)
        raw = dict(v=1, type="metrics", stream="raw", time=5, metrics={"x": 2})
        self.assertEqual(importer.parse_line(json.dumps(raw), 22)[0]["metrics"], {"x": 2})

    def test_invalid_export_records(self):
        for value in ({"type": "sample", "origin": "legacy"},
                      {"type": "metrics", "origin": "legacy", "stream": "x", "clock": "source_line", "time": 1, "unit": "us", "aggregation": "window_mean", "metrics": {"x": float("nan")}},
                      {"v": 3}, {"origin": "other"}):
            row = self.parser.parse_line(json.dumps(value), 1)[0]
            self.assertEqual(row["type"], "event")
        self.assertEqual(self.parser.parse_line('{broken', 1)[0]["name"], "malformed_record")

    def test_remaining_legacy_streams(self):
        for kind, text in [("fsr", "gpu=2.0ms resets=4"), ("taau", "gpu=2.2ms resets=5"), ("dynres", "scale_avg=0.75 gpu_avg=12.3ms changes=4")]:
            rows = self.parser.parse_line(f"DEKO9 {kind} frames=60 {text}", 1)
            self.assertTrue(rows)
            if kind != "dynres":
                self.assertEqual(rows[0]["metrics"], {"gpu": float(text.split('=')[1].split('ms')[0])})
                self.assertEqual(rows[1]["unit"], "mixed_unknown")
        self.assertEqual(self.parser.parse_line("unrelated log line", 1), [])


if __name__ == "__main__":
    unittest.main()
