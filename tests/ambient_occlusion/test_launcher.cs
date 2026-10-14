// Compiled with the production launcher. Uses isolated fixtures; never launches a game.
using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace BlackXboxLauncher
{
    internal static class AmbientOcclusionTests
    {
        private static int checks;
        private static void Check(bool condition, string message)
        { if (!condition) throw new Exception(message); checks++; }
        private static Control Find(Control root, string accessibleName)
        {
            if (root.AccessibleName == accessibleName) return root;
            foreach (Control child in root.Controls) { var found = Find(child, accessibleName); if (found != null) return found; }
            return null;
        }
        private static void Select(ComboBox box, string label)
        {
            for (int i = 0; i < box.Items.Count; i++)
                if (box.Items[i].ToString() == label) { box.SelectedIndex = i; return; }
            throw new Exception("Missing English option: " + label);
        }
        private static LaunchOptions Options(LauncherForm form)
        { return (LaunchOptions)typeof(LauncherForm).GetMethod("Options", BindingFlags.Instance | BindingFlags.NonPublic).Invoke(form, null); }
        private static string ReadActiveLog(string path)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var reader = new StreamReader(stream)) return reader.ReadToEnd();
        }
        private static void Invalid(LaunchOptions options, string expected)
        {
            try { options.Validate(); }
            catch (ArgumentException error) { Check(error.Message.Contains(expected), "Expected invalid " + expected); return; }
            throw new Exception("Accepted invalid " + expected);
        }
        [STAThread]
        private static int Main(string[] args)
        {
            if (args.Length == 1 && args[0] == "--emit-trace")
            {
                for (int i = 0; i < 20; i++)
                {
                    Console.WriteLine("[KTRACE] " + i);
                    Console.Error.WriteLine("[DSPTRACE] " + i);
                    Console.WriteLine("[KDL] " + i);
                }
                Console.WriteLine("ordinary output"); Console.Error.WriteLine("ordinary error");
                return 0;
            }
            if (args.Length == 1 && args[0] == "--exit-stub") return 0;
            try
            {
                Check(args.Length == 1, "Expected an isolated build fixture directory.");
                string fixture = Path.Combine(Path.GetFullPath(args[0]), "fixture-" + Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(fixture);
                var defaults = new LaunchOptions();
                Check(defaults.AoMethod == "off" && defaults.AoQuality == "high", "AO defaults are Off and High.");
                Check(!defaults.VisualTraceEnabled && defaults.VisualTraceFrames == "120-240/30", "Visual tracing defaults to Off with a documented frame pattern.");
                Check(!defaults.DspTraceEnabled, "DSP state tracing defaults to Off.");
                Check(defaults.KgpuDrawLogFrames == "", "GPU draw tracing defaults to Off.");
                Check(KgpuDrawFrames.Normalize(" 001740,002 ") == "1740,2" && KgpuDrawFrames.Normalize("42") == "42",
                    "GPU draw frame intervals normalize and accept one frame.");
                foreach (string invalidDrawFrames in new[] { "0", "1,0", "1,121", "1-4", "1,2,3", "x,2", "2147483648" })
                    try { KgpuDrawFrames.Normalize(invalidDrawFrames); throw new Exception("Accepted invalid KGPU draw interval: " + invalidDrawFrames); }
                    catch (ArgumentException error) { Check(error.Message.Contains("frame"), "Clear GPU draw interval validation: " + invalidDrawFrames); }
                try { new LaunchOptions { GpuRenderer = false, KgpuDrawLogFrames = "120,2" }.Validate(); throw new Exception("Accepted KGPU tracing with CPU Kelvin."); }
                catch (InvalidOperationException error) { Check(error.Message.Contains("GPU renderer"), "GPU draw trace explains its renderer requirement."); }
                var drawSettings = LaunchOptions.FromSettingsJson("{\"KgpuDrawLogFrames\":\"120,2\"}");
                Check(drawSettings.KgpuDrawLogFrames == "120,2", "GPU draw log interval persists.");
                Check(LaunchOptions.FromSettingsJson("{\"KgpuDrawLogFrames\":\"bad\"}").KgpuDrawLogFrames == "",
                    "Malformed persisted GPU draw interval defaults Off.");
                Check(VisualTraceFrames.Normalize(" 00120-00240/030 ") == "120-240/30", "Visual trace intervals normalize decimal values.");
                Check(VisualTraceFrames.Normalize("120") == "120" && VisualTraceFrames.Normalize("120-180") == "120-180", "Single frames and ranges are accepted.");
                foreach (string invalidTrace in new[] { "", "0", "120-0", "180-120", "120/2", "120-180/0", "120-180/2/3", "0x10", "1-9223372036854775808" })
                    try { VisualTraceFrames.Normalize(invalidTrace); throw new Exception("Accepted invalid visual trace interval: " + invalidTrace); }
                    catch (ArgumentException error) { Check(error.Message.Contains("frame"), "Clear visual trace validation: " + invalidTrace); }
                Invalid(new LaunchOptions { GpuRenderer = false, VisualTraceEnabled = true, VisualTraceFrames = "bad" }, "frame");
                try { new LaunchOptions { VisualTraceEnabled = true, GpuRenderer = true }.Validate(); throw new Exception("Accepted detailed tracing with the GPU renderer."); }
                catch (InvalidOperationException error) { Check(error.Message.Contains("CPU Kelvin"), "Detailed tracing explains its CPU renderer requirement."); }
                var traceSettings = LaunchOptions.FromSettingsJson("{\"VisualTraceEnabled\":true,\"VisualTraceFrames\":\"120-180/30\"}");
                Check(traceSettings.VisualTraceEnabled && traceSettings.VisualTraceFrames == "120-180/30", "Visual trace settings persist.");
                var malformedTraceSettings = LaunchOptions.FromSettingsJson("{\"VisualTraceEnabled\":true,\"VisualTraceFrames\":\"bad\"}");
                Check(malformedTraceSettings.VisualTraceEnabled && malformedTraceSettings.VisualTraceFrames == defaults.VisualTraceFrames, "Malformed persisted intervals are repaired.");
                foreach (string malformedVisualField in new[] { "\"VisualTraceEnabled\":\"yes\"", "\"VisualTraceEnabled\":{}", "\"VisualTraceFrames\":{}", "\"VisualTraceFrames\":[120]" })
                {
                    var independentlyRepaired = LaunchOptions.FromSettingsJson("{" + malformedVisualField + ",\"Threads\":7,\"AoMethod\":\"gtao\"}");
                    Check(!independentlyRepaired.VisualTraceEnabled && independentlyRepaired.VisualTraceFrames == defaults.VisualTraceFrames &&
                        independentlyRepaired.Threads == 7 && independentlyRepaired.AoMethod == "gtao",
                        "Malformed visual trace field does not drop other settings: " + malformedVisualField);
                }
                Check(!LaunchOptions.FromSettingsJson("{} ").VisualTraceEnabled, "Older launcher settings keep tracing disabled.");
                {
                    var blurOff = new LaunchOptions { ActionBlur = "off" };
                    blurOff.Validate();
                    var blurParsed = new LaunchOptions();
                    Check(blurOff.VideoIni().Contains("action_blur=off\n") && blurParsed.ApplyVideoIni(blurOff.VideoIni()) == 18 &&
                        blurParsed.ActionBlur == "off" && blurParsed.MotionBlur == "original" && blurParsed.DepthOfField == "off",
                        "Action blur round trips independently of motion blur and depth of field.");
                    Check(defaults.ActionBlur == "original", "Action blur defaults to the original effect.");
                }
                Check(LaunchOptions.FromSettingsJson("{\"DspTraceEnabled\":true}").DspTraceEnabled,
                    "DSP trace setting persists without a schema migration.");
                var malformedDspTraceSettings = LaunchOptions.FromSettingsJson("{\"DspTraceEnabled\":\"yes\",\"Threads\":7}");
                Check(!malformedDspTraceSettings.DspTraceEnabled && malformedDspTraceSettings.Threads == 7,
                    "Malformed DSP trace settings default Off without dropping other options.");
                foreach (string method in new[] { "off", "ssao", "hbao", "hbao_plus", "gtao" })
                    foreach (string quality in new[] { "low", "medium", "high", "ultra" })
                    {
                        var chosen = new LaunchOptions { AoMethod = method, AoQuality = quality };
                        chosen.Validate();
                        Check(GameSession.CreateStartInfo(fixture, chosen) != null, "Accepted AO " + method + "/" + quality);
                        var parsed = new LaunchOptions();
                        Check(parsed.ApplyVideoIni(chosen.VideoIni()) == 18 && parsed.AoMethod == method && parsed.AoQuality == quality,
                            "Shared video.ini round trip: " + method + "/" + quality);
                        var restored = LaunchOptions.FromSettingsJson(new JavaScriptSerializer().Serialize(chosen));
                        Check(restored.AoMethod == method && restored.AoQuality == quality, "Launcher JSON preserves AO choices.");
                        Check(chosen.RasterSize()[0] == defaults.RasterSize()[0] && chosen.RasterSize()[1] == defaults.RasterSize()[1],
                            "AO does not change internal resolution or the SSAA raster.");
                    }
                var read = new LaunchOptions();
                foreach (string quality in new[] { "low", "medium", "high", "ultra" })
                    Check(read.ApplyVideoIni("ssao=" + quality + "\n") == 1 && read.AoMethod == "ssao" && read.AoQuality == quality,
                        "Legacy SSAO migration: " + quality);
                Check(read.ApplyVideoIni("ssao=off\n") == 1 && read.AoMethod == "off" && read.AoQuality == "ultra", "Legacy Off retains quality.");
                Check(read.ApplyVideoIni("ssao=low\nao_method=HBAO_PLUS\nao_quality=HIGH\n") == 3 && read.AoMethod == "hbao_plus" && read.AoQuality == "high",
                    "Explicit AO keys override an earlier legacy key case-insensitively.");
                Check(read.ApplyVideoIni("ao_method=invalid\nao_quality=off\nssao=invalid\n") == 0 && read.AoMethod == "hbao_plus" && read.AoQuality == "high",
                    "Malformed AO entries preserve existing choices.");
                Invalid(new LaunchOptions { AoMethod = "invalid" }, "AoMethod");
                Invalid(new LaunchOptions { AoMethod = null }, "AoMethod");
                Invalid(new LaunchOptions { AoQuality = "off" }, "AoQuality");
                Invalid(new LaunchOptions { AoQuality = null }, "AoQuality");
                var old = LaunchOptions.FromSettingsJson("{\"Threads\":7}");
                Check(old.AoMethod == "off" && old.AoQuality == "high" && old.Threads == 7, "Legacy launcher files acquire AO defaults.");
                var repaired = LaunchOptions.FromSettingsJson("{\"AoMethod\":\"gtao\",\"AoQuality\":\"invalid\"}");
                Check(repaired.AoMethod == "gtao" && repaired.AoQuality == "high", "Malformed quality is repaired independently.");
                var culture = Thread.CurrentThread.CurrentCulture;
                try
                {
                    Thread.CurrentThread.CurrentCulture = new CultureInfo("de-DE");
                    var chosen = new LaunchOptions { AoMethod = "gtao", AoQuality = "ultra", VerticalFov = 72.5 };
                    var parsed = new LaunchOptions(); parsed.ApplyVideoIni(chosen.VideoIni());
                    Check(parsed.AoMethod == "gtao" && parsed.AoQuality == "ultra" && parsed.VerticalFov == 72.5, "AO and FOV file values are culture-independent.");
                }
                finally { Thread.CurrentThread.CurrentCulture = culture; }
                var process = GameSession.CreateStartInfo(fixture, new LaunchOptions { AoMethod = "gtao", AoQuality = "ultra" });
                Check(process.EnvironmentVariables["RECOMP_VIDEO_INI"] == LaunchOptions.VideoIniPath(fixture), "Launcher uses the shared video.ini.");
                Check(!process.EnvironmentVariables.ContainsKey("RECOMP_AO_METHOD") && !process.EnvironmentVariables.ContainsKey("RECOMP_AO_QUALITY"),
                    "Launcher choices remain editable from the in-game Video Settings page.");
                Environment.SetEnvironmentVariable("RECOMP_KELVIN_TRACE", "999");
                var normalTraceProcess = GameSession.CreateStartInfo(fixture, new LaunchOptions());
                Check(!normalTraceProcess.EnvironmentVariables.ContainsKey("RECOMP_KELVIN_TRACE") && !normalTraceProcess.EnvironmentVariables.ContainsKey("RECOMP_KELVIN_TRACE_TEX"), "Tracing stays absent by default despite inherited variables.");
                Environment.SetEnvironmentVariable("RECOMP_KGPU_DRAWLOG", "9,2");
                normalTraceProcess = GameSession.CreateStartInfo(fixture, new LaunchOptions());
                Check(!normalTraceProcess.EnvironmentVariables.ContainsKey("RECOMP_KGPU_DRAWLOG"), "Inherited KGPU draw tracing is scrubbed when disabled.");
                var enabledDrawLogProcess = GameSession.CreateStartInfo(fixture, new LaunchOptions { KgpuDrawLogFrames = "00120,02" });
                Check(enabledDrawLogProcess.EnvironmentVariables["RECOMP_KGPU_DRAWLOG"] == "120,2", "Enabled KGPU draw interval reaches the runtime normalized.");
                Environment.SetEnvironmentVariable("RECOMP_KGPU_DRAWLOG", null);
                Check(!normalTraceProcess.EnvironmentVariables.ContainsKey("RECOMP_APU_TRACE_DSP_STATE"), "DSP state tracing stays absent by default.");
                Environment.SetEnvironmentVariable("RECOMP_APU_TRACE_DSP_STATE", "1");
                normalTraceProcess = GameSession.CreateStartInfo(fixture, new LaunchOptions());
                Check(!normalTraceProcess.EnvironmentVariables.ContainsKey("RECOMP_APU_TRACE_DSP_STATE"), "Inherited DSP tracing is scrubbed when disabled.");
                var enabledDspTraceProcess = GameSession.CreateStartInfo(fixture, new LaunchOptions { DspTraceEnabled = true });
                Check(enabledDspTraceProcess.EnvironmentVariables["RECOMP_APU_TRACE_DSP_STATE"] == "1", "Enabled DSP tracing reaches the runtime.");
                Environment.SetEnvironmentVariable("RECOMP_APU_TRACE_DSP_STATE", null);
                var enabledTraceProcess = GameSession.CreateStartInfo(fixture, new LaunchOptions { GpuRenderer = false, VisualTraceEnabled = true, VisualTraceFrames = "00120-00240/030" });
                Check(enabledTraceProcess.EnvironmentVariables["RECOMP_KELVIN_TRACE"] == "120-240/30" && !enabledTraceProcess.EnvironmentVariables.ContainsKey("RECOMP_KELVIN_TRACE_TEX"), "Enabled trace exports only the normalized interval.");
                Environment.SetEnvironmentVariable("RECOMP_KELVIN_TRACE", null);
                using (var traceLog = new VisualTraceLog(Path.Combine(fixture, "visual-trace.log"), 128))
                {
                    for (int i = 0; i < 20; i++) traceLog.WriteLine("[KTRACE] " + new string('x', 30));
                    traceLog.Complete();
                    traceLog.Dispose();
                    string bounded = File.ReadAllText(Path.Combine(fixture, "visual-trace.log"));
                    Check(new FileInfo(Path.Combine(fixture, "visual-trace.log")).Length <= 128, "Visual trace log respects its configured byte cap.");
                    Check(bounded.Contains("TRUNCATED") && bounded.Contains("lines discarded"), "Capped trace log reports truncation and discarded line count.");
                }
                using (var dspTraceLog = new VisualTraceLog(Path.Combine(fixture, "audio-dsp-trace.log"), 128, "[DSPTRACE]"))
                {
                    for (int i = 0; i < 20; i++) dspTraceLog.WriteLine("[DSPTRACE] " + new string('x', 30));
                    dspTraceLog.Dispose();
                    string bounded = File.ReadAllText(Path.Combine(fixture, "audio-dsp-trace.log"));
                    Check(new FileInfo(Path.Combine(fixture, "audio-dsp-trace.log")).Length <= 128, "DSP trace log respects its configured byte cap.");
                    Check(bounded.Contains("[DSPTRACE] TRUNCATED:") && bounded.Contains("lines discarded"), "DSP trace truncation is identified with discarded line count.");
                }
                using (var gpuTraceLog = new VisualTraceLog(Path.Combine(fixture, "kgpu-draw-trace.log"), 128, "[KDL]"))
                {
                    for (int i = 0; i < 20; i++) gpuTraceLog.WriteLine("[KDL] " + new string('x', 30));
                    gpuTraceLog.Dispose();
                    string bounded = File.ReadAllText(Path.Combine(fixture, "kgpu-draw-trace.log"));
                    Check(new FileInfo(Path.Combine(fixture, "kgpu-draw-trace.log")).Length <= 128, "GPU draw trace respects its configured byte cap.");
                    Check(bounded.Contains("[KDL] TRUNCATED:") && bounded.Contains("lines discarded"), "GPU draw trace truncation reports discarded lines.");
                }
                string routeDir = Path.Combine(fixture, "routing"); Directory.CreateDirectory(routeDir);
                using (var route = new GameSession())
                {
                    var sessionLog = new StreamWriter(Path.Combine(routeDir, "session.log"), false, new System.Text.UTF8Encoding(false));
                    var visualLog = new VisualTraceLog(Path.Combine(routeDir, "visual-trace.log"));
                    var dspLog = new VisualTraceLog(Path.Combine(routeDir, "audio-dsp-trace.log"), VisualTraceLog.DefaultLimit, "[DSPTRACE]");
                    var gpuDrawLog = new VisualTraceLog(Path.Combine(routeDir, "kgpu-draw-trace.log"), VisualTraceLog.DefaultLimit, "[KDL]");
                    typeof(GameSession).GetField("log", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(route, sessionLog);
                    typeof(GameSession).GetField("visualTraceLog", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(route, visualLog);
                    typeof(GameSession).GetField("dspTraceLog", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(route, dspLog);
                    typeof(GameSession).GetField("kgpuDrawTraceLog", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(route, gpuDrawLog);
                    route.WriteOutput("[KTRACE] draw details", false); route.WriteOutput("[DSPTRACE] core=GP event=SEN", false); route.WriteOutput("[KDL] f120 #1 draw vs 463DBDF0", false); route.WriteOutput("ordinary output", false); route.WriteOutput("ordinary error", true);
                    route.Dispose();
                    Check(File.ReadAllText(Path.Combine(routeDir, "session.log")) == "ordinary output" + Environment.NewLine + "[stderr] ordinary error" + Environment.NewLine,
                        "KTRACE lines are not duplicated in session.log.");
                    Check(File.ReadAllText(Path.Combine(routeDir, "visual-trace.log")) == "[KTRACE] draw details" + Environment.NewLine,
                        "KTRACE lines are routed to the separate log.");
                    Check(File.ReadAllText(Path.Combine(routeDir, "audio-dsp-trace.log")) == "[DSPTRACE] core=GP event=SEN" + Environment.NewLine,
                        "DSP state trace is routed separately from session.log.");
                    Check(File.ReadAllText(Path.Combine(routeDir, "kgpu-draw-trace.log")) == "[KDL] f120 #1 draw vs 463DBDF0" + Environment.NewLine,
                        "KGPU draw trace is routed separately from session.log.");
                }
                foreach (bool failCompletion in new[] { false, true })
                {
                    string lifecycleDir = Path.Combine(fixture, failCompletion ? "failed-completion" : "completed-output"); Directory.CreateDirectory(lifecycleDir);
                    using (var lifecycle = new GameSession())
                    {
                        var visualLog = new VisualTraceLog(Path.Combine(lifecycleDir, "visual-trace.log"), 128);
                        var dspLog = new VisualTraceLog(Path.Combine(lifecycleDir, "audio-dsp-trace.log"), 128, "[DSPTRACE]");
                        var drawLog = new VisualTraceLog(Path.Combine(lifecycleDir, "kgpu-draw-trace.log"), 128, "[KDL]");
                        typeof(GameSession).GetProperty("DirectoryPath", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(lifecycle, lifecycleDir, null);
                        typeof(GameSession).GetField("visualTraceLog", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(lifecycle, visualLog);
                        typeof(GameSession).GetField("dspTraceLog", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(lifecycle, dspLog);
                        typeof(GameSession).GetField("kgpuDrawTraceLog", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(lifecycle, drawLog);
                        var child = new Process { StartInfo = new ProcessStartInfo {
                            FileName = Assembly.GetExecutingAssembly().Location, Arguments = failCompletion ? "--exit-stub" : "--emit-trace",
                            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true
                        } };
                        typeof(GameSession).GetField("process", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(lifecycle, child);
                        child.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null) lifecycle.WriteOutput(e.Data, false); };
                        child.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null) lifecycle.WriteOutput(e.Data, true); };
                        if (failCompletion)
                        {
                            visualLog.WriteLine("[KTRACE] dropped line");
                            ((StreamWriter)typeof(VisualTraceLog).GetField("writer", BindingFlags.Instance | BindingFlags.NonPublic).GetValue(visualLog)).Dispose();
                        }
                        child.Start(); child.BeginOutputReadLine(); child.BeginErrorReadLine();
                        Check(lifecycle.WaitForExit() == 0, "Isolated output stub exits normally.");
                        lifecycle.ArchiveKernelLog(fixture);
                        var exit = new JavaScriptSerializer().Deserialize<System.Collections.Generic.Dictionary<string, object>>(File.ReadAllText(Path.Combine(lifecycleDir, "exit.json")));
                        if (failCompletion)
                            Check(lifecycle.LogFailure != null && (string)exit["logFailure"] == lifecycle.LogFailure,
                                "Trace completion failures are recorded before session exit metadata is saved.");
                        else
                            foreach (string file in new[] { "visual-trace.log", "audio-dsp-trace.log", "kgpu-draw-trace.log" })
                                Check(ReadActiveLog(Path.Combine(lifecycleDir, file)).Contains("20 lines discarded") && new FileInfo(Path.Combine(lifecycleDir, file)).Length <= 128,
                                    "Redirected output is drained and finalized before session exit metadata: " + file);
                    }
                }

                string project = Path.Combine(fixture, "AO UI");
                Directory.CreateDirectory(Path.Combine(project, "local"));
                using (var form = new LauncherForm(project, true))
                {
                    var traceToggle = Find(form, "Detailed visual trace") as CheckBox;
                    var dspTraceToggle = Find(form, "Detailed DSP state trace (GP/EP stack)") as CheckBox;
                    var gpuDrawFrames = Find(form, "GPU draw log frames") as TextBox;
                    var traceFrames = Find(form, "Visual trace frames") as TextBox;
                    Check(traceToggle != null && traceFrames != null && !traceToggle.Checked && !traceFrames.Enabled,
                        "Game settings expose visual tracing disabled by default.");
                    traceToggle.Checked = true; traceFrames.Text = "120-240/30";
                    Check(traceFrames.Enabled && Options(form).VisualTraceEnabled && Options(form).VisualTraceFrames == "120-240/30",
                        "Visual trace UI interval reaches the launch settings.");
                    Check(dspTraceToggle != null && !dspTraceToggle.Checked, "DSP trace UI is visible and disabled by default.");
                    Check(gpuDrawFrames != null && gpuDrawFrames.Text == "", "GPU draw log UI is empty and disabled by default.");
                    if (gpuDrawFrames != null) { gpuDrawFrames.Text = "120,2"; Check(Options(form).KgpuDrawLogFrames == "120,2", "GPU draw interval UI reaches launch settings."); }
                    dspTraceToggle.Checked = true;
                    Check(Options(form).DspTraceEnabled, "DSP trace UI reaches launch settings.");
                    var method = Find(form, "Ambient occlusion method") as ComboBox;
                    var quality = Find(form, "Ambient occlusion quality") as ComboBox;
                    Check(method != null && quality != null && method.Items.Count == 5 && quality.Items.Count == 4, "All English AO options are exposed.");
                    Check(!quality.Enabled && quality.SelectedItem.ToString() == "High", "Quality is remembered while AO is Off.");
                    foreach (string label in new[] { "SSAO Classic", "HBAO", "HBAO+", "GTAO" })
                    { Select(method, label); Check(quality.Enabled, "Quality is enabled for " + label); }
                    Select(quality, "Ultra");
                    Check(Options(form).AoMethod == "gtao" && Options(form).AoQuality == "ultra", "UI choices reach the production settings model.");
                    File.WriteAllText(LaunchOptions.VideoIniPath(project), "ao_method=hbao_plus\nao_quality=medium\n");
                    typeof(LauncherForm).GetMethod("ReloadVideo", BindingFlags.Instance | BindingFlags.NonPublic).Invoke(form, null);
                    Check(Options(form).AoMethod == "hbao_plus" && Options(form).AoQuality == "medium" && quality.Enabled, "Reload AO choices saved by the game.");
                    Select(method, "Off");
                    Check(!quality.Enabled && Options(form).AoQuality == "medium", "Turning AO Off preserves quality.");
                    var tip = (ToolTip)typeof(LauncherForm).GetField("settingsTip", BindingFlags.Instance | BindingFlags.NonPublic).GetValue(form);
                    Check(tip.GetToolTip(quality).Contains("sampling density and radius") && tip.GetToolTip(quality).Contains("scene resolution"),
                        "Quality help describes actual sampling behavior.");
                }
                File.WriteAllText(Path.Combine(project, "local", "xbox-launcher.json"), "{\"DspTraceEnabled\":true,\"Threads\":7}");
                using (var restoredForm = new LauncherForm(project, true))
                {
                    var restoredDspTrace = Find(restoredForm, "Detailed DSP state trace (GP/EP stack)") as CheckBox;
                    Check(restoredDspTrace != null && restoredDspTrace.Checked && Options(restoredForm).DspTraceEnabled && Options(restoredForm).Threads == 7,
                        "Reopening the launcher restores the saved DSP trace choice.");
                    Check(restoredForm.Height <= 728 && restoredForm.MinimumSize.Height <= 728,
                        "Launcher defaults and minimum size fit a 768-pixel screen with a taskbar.");
                    var tabs = (TabControl)typeof(LauncherForm).GetField("settingsTabs", BindingFlags.Instance | BindingFlags.NonPublic).GetValue(restoredForm);
                    foreach (TabPage tab in tabs.TabPages)
                        Check(tab.AutoScroll, "Resizing retains access to settings on the " + tab.Text + " tab.");
                }
                LanguageBankTests.Run(fixture, Check);
                LauncherLanguageTests.Run(fixture, Check);
                Console.WriteLine("PASS: " + checks + " actual-launcher checks.");
                return 0;
            }
            catch (Exception error) { Console.Error.WriteLine("FAIL: " + error); return 1; }
        }
    }
}
