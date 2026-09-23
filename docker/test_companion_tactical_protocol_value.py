import pathlib, shutil, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parent.parent
class TacticalProtocolValueTest(unittest.TestCase):
    def test_value_contract(self):
        compiler=shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            probe=subprocess.run(["wsl","-e","bash","-lc","command -v g++ || command -v clang++"],capture_output=True,text=True)
            if probe.returncode != 0: self.skipTest("no compiler")
            cmd=["wsl","-e","bash","-lc",f"{probe.stdout.strip()} -std=c++17 -Wall -Wextra -I /mnt/c/Users/hpark/WebstormProjects/tortoise-wow/src/game/PlayerBots -o /tmp/tactical /mnt/c/Users/hpark/WebstormProjects/tortoise-wow/docker/test_companion_tactical_protocol_value.cpp && /tmp/tactical"]
            r=subprocess.run(cmd,capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr); self.assertIn("ALL OK",r.stdout); return
        with tempfile.TemporaryDirectory() as d:
            exe=pathlib.Path(d)/"tactical.exe"
            b=subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-I",str(ROOT/"src/game/PlayerBots"),"-o",str(exe),str(ROOT/"docker/test_companion_tactical_protocol_value.cpp")],capture_output=True,text=True)
            self.assertEqual(b.returncode,0,b.stdout+b.stderr)
            r=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stdout+r.stderr)
            self.assertIn("ALL OK",r.stdout)
if __name__ == "__main__": unittest.main()
