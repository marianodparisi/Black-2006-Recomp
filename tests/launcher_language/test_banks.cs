// Synthetic fixtures exercise language-bank migration without retail assets.
using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace BlackXboxLauncher
{
    internal static class LanguageBankTests
    {
        private static readonly byte[] English = Encoding.UTF8.GetBytes("synthetic English bank\n");
        private static readonly byte[] Portuguese = Encoding.UTF8.GetBytes("synthetic Portuguese bank\n");
        private static readonly byte[] Unknown = Encoding.UTF8.GetBytes("unrecognized installed bank\n");

        internal static void Run(string fixtureRoot, Action<bool, string> check)
        {
            string fixture = Path.Combine(fixtureRoot, "language-banks");
            Directory.CreateDirectory(fixture);
            string stockHash;
            using (var hash = SHA256.Create())
                stockHash = BitConverter.ToString(hash.ComputeHash(English)).Replace("-", "");

            string root = CreateGame(fixture, "fresh-english", English);
            GameLanguageBanks.Install(root, GameLanguageBanks.English, stockHash);
            Same(GameLanguageBanks.GamePath(root), English, check, "First English launch keeps the installed English bank.");
            Same(GameLanguageBanks.BankPath(root, GameLanguageBanks.English), English, check, "First English launch preserves its source automatically.");
            Clean(root, check);

            root = CreateGame(fixture, "fresh-read-only-english", English);
            string originalTarget = GameLanguageBanks.GamePath(root);
            File.SetAttributes(originalTarget, FileAttributes.ReadOnly);
            DateTime targetWriteTime = File.GetLastWriteTimeUtc(originalTarget);
            GameLanguageBanks.Install(root, GameLanguageBanks.English, stockHash);
            Same(originalTarget, English, check, "Fresh English bootstrap keeps a read-only retail target intact.");
            check(File.GetLastWriteTimeUtc(originalTarget) == targetWriteTime && (File.GetAttributes(originalTarget) & FileAttributes.ReadOnly) != 0,
                "Fresh English bootstrap preserves the retail target's attributes and timestamp.");
            Same(GameLanguageBanks.BankPath(root, GameLanguageBanks.English), English, check, "Fresh English bootstrap preserves a read-only target as its fallback.");
            GameLanguageBanks.Install(root, GameLanguageBanks.English);
            Same(originalTarget, English, check, "Repeated English launch keeps the read-only retail target intact.");
            check(File.GetLastWriteTimeUtc(originalTarget) == targetWriteTime && (File.GetAttributes(originalTarget) & FileAttributes.ReadOnly) != 0,
                "Repeated English launch preserves the retail target's attributes and timestamp.");
            Clean(root, check);

            root = CreateGame(fixture, "first-portuguese", English);
            Stage(root, GameLanguageBanks.PortugueseBrazil, Portuguese);
            GameLanguageBanks.Install(root, GameLanguageBanks.PortugueseBrazil, stockHash);
            Same(GameLanguageBanks.GamePath(root), Portuguese, check, "First Portuguese launch installs the selected bank.");
            Same(GameLanguageBanks.BankPath(root, GameLanguageBanks.English), English, check, "English is preserved before the first Portuguese switch.");
            string portugueseTarget = GameLanguageBanks.GamePath(root);
            File.SetAttributes(portugueseTarget, FileAttributes.ReadOnly);
            DateTime portugueseWriteTime = File.GetLastWriteTimeUtc(portugueseTarget);
            GameLanguageBanks.Install(root, GameLanguageBanks.PortugueseBrazil);
            Same(portugueseTarget, Portuguese, check, "Repeated Portuguese launch leaves the selected bank intact.");
            check(File.GetLastWriteTimeUtc(portugueseTarget) == portugueseWriteTime && (File.GetAttributes(portugueseTarget) & FileAttributes.ReadOnly) != 0,
                "Repeated Portuguese launch avoids replacing an identical read-only bank.");
            File.SetAttributes(portugueseTarget, FileAttributes.Normal);
            GameLanguageBanks.Install(root, GameLanguageBanks.English);
            Same(GameLanguageBanks.GamePath(root), English, check, "Production installer restores the preserved English bank.");
            Same(GameLanguageBanks.BankPath(root, GameLanguageBanks.PortugueseBrazil), Portuguese, check, "Switching languages retains the Portuguese source.");
            Clean(root, check);

            root = CreateGame(fixture, "staged-english", Unknown);
            Stage(root, GameLanguageBanks.English, English);
            Stage(root, GameLanguageBanks.PortugueseBrazil, Portuguese);
            string englishPath = GameLanguageBanks.BankPath(root, GameLanguageBanks.English);
            DateTime originalWriteTime = File.GetLastWriteTimeUtc(englishPath);
            File.SetAttributes(englishPath, FileAttributes.ReadOnly);
            GameLanguageBanks.Install(root, GameLanguageBanks.PortugueseBrazil);
            Same(englishPath, English, check, "A staged English source is never overwritten by the current installed bank.");
            check(File.GetLastWriteTimeUtc(englishPath) == originalWriteTime && (File.GetAttributes(englishPath) & FileAttributes.ReadOnly) != 0,
                "A staged English source retains its timestamp and read-only attribute.");
            GameLanguageBanks.Install(root, GameLanguageBanks.English);
            Same(GameLanguageBanks.GamePath(root), English, check, "The production installer accepts an existing English fallback from a translated install.");
            Clean(root, check);

            root = CreateGame(fixture, "missing-portuguese", English);
            Expect<FileNotFoundException>(delegate { GameLanguageBanks.Install(root, GameLanguageBanks.PortugueseBrazil, stockHash); }, check, "pt-BR");
            Same(GameLanguageBanks.GamePath(root), English, check, "A missing Portuguese source leaves stock English installed.");
            check(!File.Exists(GameLanguageBanks.BankPath(root, GameLanguageBanks.English)), "A missing Portuguese source causes no bootstrap writes.");
            Clean(root, check);

            foreach (string language in new[] { GameLanguageBanks.English, GameLanguageBanks.PortugueseBrazil })
            {
                root = CreateGame(fixture, "unknown-" + language, Unknown);
                if (language == GameLanguageBanks.PortugueseBrazil) Stage(root, language, Portuguese);
                string unknownRoot = root;
                Expect<InvalidDataException>(delegate { GameLanguageBanks.Install(unknownRoot, language, stockHash); }, check, "local\\language-banks\\en-US");
                Same(GameLanguageBanks.GamePath(root), Unknown, check, "An unknown installed bank is retained when the English fallback is missing.");
                check(!File.Exists(GameLanguageBanks.BankPath(root, GameLanguageBanks.English)), "An unknown installed bank is never mislabeled as English.");
                Clean(root, check);
            }

            root = CreateGame(fixture, "production-hash", English);
            Expect<InvalidDataException>(delegate { GameLanguageBanks.Install(root, GameLanguageBanks.English); }, check, "original English");
            Same(GameLanguageBanks.GamePath(root), English, check, "Production hash validation rejects a synthetic bank without replacing it.");
            check(!File.Exists(GameLanguageBanks.BankPath(root, GameLanguageBanks.English)), "Production bootstrap uses the fixed stock English digest.");
            Clean(root, check);

            root = Path.Combine(fixture, "missing-game-bank");
            Stage(root, GameLanguageBanks.English, English);
            Expect<FileNotFoundException>(delegate { GameLanguageBanks.Install(root, GameLanguageBanks.English); }, check, "game\\language\\strings");
            Same(GameLanguageBanks.BankPath(root, GameLanguageBanks.English), English, check, "A missing target does not change the staged English source.");
            Clean(root, check);

            ReleaseLanguage(fixture, check);
        }

        // The PAL disc picks its official bank from the console language.
        private static void ReleaseLanguage(string fixture, Action<bool, string> check)
        {
            check(GameRelease.FromSha256(GameRelease.NtscSha256.ToLowerInvariant()) == GameRelease.Ntsc, "The NTSC XBE hash is recognized.");
            check(GameRelease.FromSha256(GameRelease.PalSha256) == GameRelease.Pal, "The PAL XBE hash is recognized.");
            check(GameRelease.FromSha256(new string('0', 64)) == null, "Other XBE hashes are not supported.");
            check(GameRelease.Detect(Path.Combine(fixture, "no-game")) == null, "A missing XBE has no release.");

            check(GameLanguageBanks.ConsoleLanguage(GameLanguageBanks.Spanish) == 5, "Spanish selects console language 5 (MainSp).");
            check(GameLanguageBanks.ConsoleLanguage(GameLanguageBanks.French) == 4, "French selects console language 4 (MainFr).");
            check(GameLanguageBanks.ConsoleLanguage(GameLanguageBanks.German) == 3, "German selects console language 3 (MainGe).");
            check(GameLanguageBanks.ConsoleLanguage(GameLanguageBanks.Italian) == 6, "Italian selects console language 6 (MainIt).");
            check(GameLanguageBanks.ConsoleLanguage(GameLanguageBanks.English) == 1 &&
                  GameLanguageBanks.ConsoleLanguage(GameLanguageBanks.PortugueseBrazil) == 1, "Languages without a PAL bank keep English.");

            check(GameLanguageBanks.DisplayName(GameLanguageBanks.Spanish, GameRelease.Pal).Contains("official") &&
                  GameLanguageBanks.DisplayName(GameLanguageBanks.Spanish, GameRelease.Ntsc) == GameLanguageBanks.DisplayName(GameLanguageBanks.Spanish),
                  "Only PAL describes its banks as official.");
        }

        private static string CreateGame(string fixture, string name, byte[] bank)
        {
            string root = Path.Combine(fixture, name);
            string target = GameLanguageBanks.GamePath(root);
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            File.WriteAllBytes(target, bank);
            return root;
        }

        private static void Stage(string root, string language, byte[] bank)
        {
            string path = GameLanguageBanks.BankPath(root, language);
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            File.WriteAllBytes(path, bank);
        }

        private static void Same(string path, byte[] expected, Action<bool, string> check, string message)
        {
            check(File.Exists(path) && Convert.ToBase64String(File.ReadAllBytes(path)) == Convert.ToBase64String(expected), message);
        }

        private static void Clean(string root, Action<bool, string> check)
        {
            check(Directory.GetFiles(root, "*.launcher-tmp-*", SearchOption.AllDirectories).Length == 0, "Language installation leaves no temporary bank files.");
        }

        private static void Expect<T>(Action action, Action<bool, string> check, string messagePart) where T : Exception
        {
            try { action(); }
            catch (T error) { check(error.Message.Contains(messagePart), "Language-bank failure explains how to restore the missing source."); return; }
            throw new Exception("Expected " + typeof(T).Name + " from language-bank installation.");
        }
    }
}
