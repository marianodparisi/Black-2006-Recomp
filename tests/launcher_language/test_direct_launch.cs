// Compiled with the production launcher. Gameplay is a synthetic child executable.
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace BlackXboxLauncher
{
    internal static class LauncherDirectTests
    {
        private static int checks;
        private static void Check(bool value, string description)
        {
            if (!value) throw new Exception(description);
            checks++;
        }

        [STAThread]
        private static int Main(string[] args)
        {
            if (Path.GetFileNameWithoutExtension(Assembly.GetExecutingAssembly().Location) == "black_xbox_recomp")
            {
                Console.WriteLine("synthetic game started");
                Console.Error.WriteLine("synthetic diagnostic");
                File.WriteAllText(Path.Combine(Environment.CurrentDirectory, "synthetic-game.json"),
                    new JavaScriptSerializer().Serialize(new {
                        threads = Environment.GetEnvironmentVariable("RECOMP_KELVIN_THREADS"),
                        language = Environment.GetEnvironmentVariable("RECOMP_LANGUAGE"),
                        video = Environment.GetEnvironmentVariable("RECOMP_VIDEO_INI")
                    }));
                Thread.Sleep(100);
                return 0;
            }
            try
            {
                Check(args.Length >= 2 && args.Length <= 4, "Expected fixture and staged launcher directories, optionally an owned game root and legacy launcher flag.");
                string fixture = Path.Combine(Path.GetFullPath(args[0]), "direct-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(fixture);
                Check(!Program.LoadOptions(fixture).SkipMovies, "A clean direct launch keeps current movie defaults.");
                string project = Path.Combine(fixture, "workspace with spaces", "black-xbox", "game");
                Directory.CreateDirectory(Path.Combine(project, "src"));
                Directory.CreateDirectory(Path.Combine(project, "scripts"));
                File.WriteAllText(Path.Combine(project, "src", "main.c"), "// synthetic project");
                Check(Program.FindProject(project) == project, "Discover a standalone project.");
                Check(Program.FindProject(Path.GetDirectoryName(project)) == project, "Discover a checkout's game project.");
                string workspace = Path.GetDirectoryName(Path.GetDirectoryName(project));
                Check(Program.FindProject(workspace) == project, "Discover the existing workspace launcher layout.");
                Directory.CreateDirectory(Path.Combine(project, "local"));
                string settings = Path.Combine(project, "local", "xbox-launcher.json");
                string video = LaunchOptions.VideoIniPath(project);
                string savedSettings = " {\"SettingsVersion\":8,\"Threads\":7,\"SkipMovies\":false,\"InternalHeight\":480,\"MouseSensitivity\":2.5,\"Bindings\":{\"FIRE\":\"Mouse4\"}} ";
                string savedVideo = "; preserve formatting and future keys\r\n[video]\r\ninternal_height=1080\r\naspect=21:9\r\nfuture_setting=yes\r\n";
                File.WriteAllText(settings, savedSettings, new UTF8Encoding(true));
                File.WriteAllText(video, savedVideo, new UTF8Encoding(true));
                byte[] settingsBytes = File.ReadAllBytes(settings), videoBytes = File.ReadAllBytes(video);
                var loaded = Program.LoadOptions(project);
                Check(loaded.Threads == 7 && loaded.MouseSensitivity == 2.5 && loaded.Bindings["FIRE"] == "Mouse4", "Direct launch keeps existing game and input preferences.");
                Check(loaded.InternalHeight == 1080 && loaded.CameraAspect == "21:9", "In-game video settings override stale launcher JSON.");
                Check(Equal(settingsBytes, File.ReadAllBytes(settings)) && Equal(videoBytes, File.ReadAllBytes(video)), "Loading preserves configuration bytes, BOM and unknown keys.");
                if (args.Length < 3)
                {
                    TestAssetFreeExecutables(project, workspace, Path.GetFullPath(args[1]), settingsBytes, videoBytes);
                    Console.WriteLine("SKIP synthetic launch lifecycle: owned retail files were not supplied.");
                }
                else
                    TestExecutables(project, workspace, Path.GetFullPath(args[1]), Path.GetFullPath(args[2]), settingsBytes, videoBytes, args.Length == 4 && args[3] == "--legacy");
                TestPortableRelease(fixture, Path.GetFullPath(args[1]), args.Length >= 3 ? Path.GetFullPath(args[2]) : null);
                Console.WriteLine("PASS: " + checks + " direct launcher checks.");
                return 0;
            }
            catch (Exception error) { Console.Error.WriteLine("FAIL: " + error); return 1; }
        }

        private static bool Equal(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
            return true;
        }

        private sealed class ProcessResult
        {
            internal int ExitCode;
            internal string StandardOutput;
            internal string StandardError;
        }

        private static ProcessResult RunDetailed(string executable, string arguments)
        {
            using (var process = Process.Start(new ProcessStartInfo {
                FileName = executable, Arguments = arguments, WorkingDirectory = Path.GetDirectoryName(executable),
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true
            }))
            {
                bool windowSeen = false;
                var clock = Stopwatch.StartNew();
                while (!process.WaitForExit(20) && clock.ElapsedMilliseconds < 10000)
                {
                    try { process.Refresh(); windowSeen |= process.MainWindowHandle != IntPtr.Zero; }
                    catch (InvalidOperationException) { if (!process.HasExited) throw; }
                }
                if (!process.HasExited) { process.Kill(); process.WaitForExit(); throw new Exception("Synthetic launcher timeout: " + arguments); }
                Check(!windowSeen, "Direct and check modes finish without a settings window.");
                return new ProcessResult {
                    ExitCode = process.ExitCode,
                    StandardOutput = process.StandardOutput.ReadToEnd(),
                    StandardError = process.StandardError.ReadToEnd()
                };
            }
        }

        private static int Run(string executable, string arguments)
        {
            return RunDetailed(executable, arguments).ExitCode;
        }

        private static void TestPortableRelease(string fixture, string staged, string ownedRoot)
        {
            string root = Path.Combine(fixture, "portable release with spaces");
            Directory.CreateDirectory(root);
            File.WriteAllText(Path.Combine(root, "black-release.json"), "{}");
            Check(Program.FindProject(root) == root, "Portable releases are found without source or scripts.");
            string legacy = GameSession.Executable(root);
            Check(legacy == Path.Combine(root, "build-vs2022", "Release", "black_xbox_recomp.exe"), "Starter releases can use the normal build output.");
            string engine = Path.Combine(root, "bin", "black_xbox_recomp.exe");
            Directory.CreateDirectory(Path.GetDirectoryName(engine));
            File.Copy(Assembly.GetExecutingAssembly().Location, engine);
            Check(GameSession.Executable(root) == engine, "A portable bin executable takes precedence.");
            string main = Path.Combine(root, "BLACK PC Launcher.exe");
            File.Copy(Path.Combine(staged, "BLACK PC Launcher.exe"), main);
            ProcessResult missing = RunDetailed(main, "--check");
            Check(missing.ExitCode == 1 && missing.StandardError.Contains("The original BLACK game executable is missing"), "Portable check discovers the release and reports missing assets.");
            Check(!Directory.Exists(Path.Combine(root, "reports")), "Portable failed checks do not create sessions.");
            if (ownedRoot == null) return;
            Directory.CreateDirectory(Path.Combine(root, "game", "language", "strings"));
            File.Copy(Path.Combine(ownedRoot, "game", "default.xbe"), Path.Combine(root, "game", "default.xbe"));
            File.Copy(GameLanguageBanks.GamePath(ownedRoot), GameLanguageBanks.GamePath(root));
            File.WriteAllText(Path.Combine(root, "game", "GlobData.bin"), "synthetic asset");
            foreach (string folder in new[] { "levels", "sound", "videos" }) Directory.CreateDirectory(Path.Combine(root, "game", folder));
            Check(Run(main, "--check") == 0, "Portable retail check succeeds from bin layout.");
            Check(Run(main, "") == 0 && File.Exists(Path.Combine(root, "synthetic-game.json")), "Portable one-click play starts its child with the release working directory.");
            Check(Directory.GetDirectories(Path.Combine(root, "reports"), "launcher-*").Length == 1, "Portable play retains session diagnostics.");
        }

        private static void TestAssetFreeExecutables(string project, string workspace, string staged,
            byte[] settingsBytes, byte[] videoBytes)
        {
            string main = Path.Combine(workspace, "BLACK PC Launcher.exe");
            string settings = Path.Combine(workspace, "BLACK PC Settings.exe");
            File.Copy(Path.Combine(staged, "BLACK PC Launcher.exe"), main);
            File.Copy(Path.Combine(staged, "BLACK PC Settings.exe"), settings);
            string engine = GameSession.Executable(project);
            Directory.CreateDirectory(Path.GetDirectoryName(engine));
            File.WriteAllText(engine, "synthetic build placeholder", Encoding.ASCII);
            Check(File.Exists(main) && File.Exists(settings), "Both publication entry points are staged in the synthetic project.");

            string reports = Path.Combine(project, "reports");
            string expectedInvalid = "Use --settings to open settings or --check to verify the game files.";
            foreach (string executable in new[] { main, settings })
            {
                ProcessResult invalid = RunDetailed(executable, "--unknown");
                Check(invalid.ExitCode == 1 && invalid.StandardError.Contains(expectedInvalid),
                    "Each publication entry point rejects an invalid switch cleanly.");
                Check(!Directory.Exists(reports) && Equal(settingsBytes, File.ReadAllBytes(Path.Combine(project, "local", "xbox-launcher.json"))) &&
                    Equal(videoBytes, File.ReadAllBytes(LaunchOptions.VideoIniPath(project))),
                    "Rejecting an invalid switch creates no report and preserves both settings files.");

                ProcessResult check = RunDetailed(executable, "--check");
                Check(check.ExitCode == 1 && check.StandardError.Contains("The original BLACK game executable is missing"),
                    "Each publication entry point reports missing retail assets without starting a session.");
                Check(!Directory.Exists(reports) && Equal(settingsBytes, File.ReadAllBytes(Path.Combine(project, "local", "xbox-launcher.json"))) &&
                    Equal(videoBytes, File.ReadAllBytes(LaunchOptions.VideoIniPath(project))),
                    "A failed asset check creates no report and preserves both settings files.");
            }
        }

        private static void TestExecutables(string project, string workspace, string staged, string ownedRoot, byte[] settingsBytes, byte[] videoBytes, bool legacy)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(GameSession.Executable(project)));
            File.Copy(Assembly.GetExecutingAssembly().Location, GameSession.Executable(project));
            Directory.CreateDirectory(Path.Combine(project, "game", "language", "strings"));
            File.Copy(Path.Combine(ownedRoot, "game", "default.xbe"), Path.Combine(project, "game", "default.xbe"));
            File.Copy(GameLanguageBanks.GamePath(ownedRoot), GameLanguageBanks.GamePath(project));
            File.WriteAllText(Path.Combine(project, "game", "GlobData.bin"), "synthetic asset");
            foreach (string folder in new[] { "levels", "sound", "videos" }) Directory.CreateDirectory(Path.Combine(project, "game", folder));
            string main = Path.Combine(workspace, "BLACK PC Launcher.exe"), settings = Path.Combine(workspace, "BLACK PC Settings.exe");
            File.Copy(Path.Combine(staged, "BLACK PC Launcher.exe"), main);
            File.Copy(Path.Combine(staged, "BLACK PC Settings.exe"), settings);
            File.Copy(main, Path.Combine(workspace, "BLACK Xbox Launcher.exe"));
            Check(Run(main, "--check") == 0 && !Directory.Exists(Path.Combine(project, "reports")), "Main check verifies assets without starting a session.");
            Check(Run(settings, "--check") == 0 && !File.Exists(Path.Combine(project, "synthetic-game.json")), "Settings check keeps the same check behavior.");
            Check(Run(main, "--unknown") == 1 && Run(main, "--settings --check") == 1 && !Directory.Exists(Path.Combine(project, "reports")), "Invalid switches are rejected without starting a game.");
            foreach (string executable in new[] { main, Path.Combine(workspace, "BLACK Xbox Launcher.exe") })
            {
                Check(Run(executable, "") == 0, "Normal and compatibility launchers start the synthetic game directly.");
                var child = new JavaScriptSerializer().Deserialize<System.Collections.Generic.Dictionary<string, object>>(File.ReadAllText(Path.Combine(project, "synthetic-game.json")));
                Check((string)child["threads"] == "7" && (string)child["language"] == (legacy ? null : GameLanguageBanks.English) && (string)child["video"] == LaunchOptions.VideoIniPath(project), "The direct child receives the saved profile and appropriate language initialization.");
                Check(Equal(settingsBytes, File.ReadAllBytes(Path.Combine(project, "local", "xbox-launcher.json"))) && Equal(videoBytes, File.ReadAllBytes(LaunchOptions.VideoIniPath(project))), "Direct startup leaves both settings files byte-identical.");
            }
            string[] reports = Directory.GetDirectories(Path.Combine(project, "reports"), "launcher-*");
            Check(reports.Length == 2, "Each direct launch creates one reviewable session.");
            foreach (string report in reports)
            {
                string log = File.ReadAllText(Path.Combine(report, "session.log"));
                Check(log.Contains("synthetic game started") && log.Contains("[stderr] synthetic diagnostic") && log.Contains("Exit code: 0"), "Background session drains both child streams and records its exit.");
                Check(Equal(videoBytes, File.ReadAllBytes(Path.Combine(report, "video.ini"))) && File.Exists(Path.Combine(report, "exit.json")), "Session evidence keeps the exact video file and exit record.");
            }
            foreach (var command in new[] { new[] { settings, "" }, new[] { main, "--settings" } })
            using (var process = Process.Start(new ProcessStartInfo { FileName = command[0], Arguments = command[1], UseShellExecute = false }))
            {
                try
                {
                    var clock = Stopwatch.StartNew();
                    while (!process.HasExited && process.MainWindowHandle == IntPtr.Zero && clock.ElapsedMilliseconds < 10000) { Thread.Sleep(20); process.Refresh(); }
                    Check(!process.HasExited && process.MainWindowHandle != IntPtr.Zero && process.MainWindowTitle == "BLACK | PC Launcher", "The settings executable and --settings open the settings window.");
                    Check(Directory.GetDirectories(Path.Combine(project, "reports"), "launcher-*").Length == 2, "Opening settings does not start a game.");
                    process.CloseMainWindow();
                    Check(process.WaitForExit(5000) && process.ExitCode == 0, "The standalone settings window closes normally.");
                }
                finally { if (!process.HasExited) { process.Kill(); process.WaitForExit(); } }
            }
        }
    }
}
