using System;
using System.IO;
using System.Security.Cryptography;

namespace BlackXboxLauncher
{
    // The retail release in game\, by default.xbe hash. scripts\black_release.py
    // describes how their layouts differ.
    internal static class GameRelease
    {
        internal const string Ntsc = "NTSC";
        internal const string Pal = "PAL";
        internal const string NtscSha256 = "DF2739C372D254A90AEECCF5971D12097F22D89FBB90DB021FDA96DD4DEB68F6";
        internal const string PalSha256 = "67F35121CCF622E7A293C402219596FE6456CA4C3D27E6F8139E449264DA30F0";

        internal static string FromSha256(string sha256)
        {
            if (String.Equals(sha256, NtscSha256, StringComparison.OrdinalIgnoreCase)) return Ntsc;
            if (String.Equals(sha256, PalSha256, StringComparison.OrdinalIgnoreCase)) return Pal;
            return null;
        }

        internal static string XbePath(string root)
        {
            return Path.Combine(Path.GetFullPath(root), "game", "default.xbe");
        }

        internal static string Sha256(string root)
        {
            using (var hash = SHA256.Create())
            using (var stream = File.OpenRead(XbePath(root)))
                return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "");
        }

        // The release, or null when default.xbe is missing or not a supported one.
        internal static string Detect(string root)
        {
            return File.Exists(XbePath(root)) ? FromSha256(Sha256(root)) : null;
        }
    }

    internal static class GameLanguageBanks
    {
        internal const string English = "en-US";
        internal const string PortugueseBrazil = "pt-BR";
        internal const string Spanish = "es";
        internal const string French = "fr";
        internal const string German = "de";
        internal const string Italian = "it";
        internal const string StockEnglishSha256 = "B23EEB4AD86A72894B40E7239D0EA8842E5A6BC05936A936401B608070E8C7BB";

        internal static bool IsSupported(string language)
        {
            return language == English || language == PortugueseBrazil || language == Spanish ||
                language == French || language == German || language == Italian;
        }

        internal static string Normalize(string language)
        {
            return IsSupported(language) ? language : English;
        }

        internal static bool IsMachineDraft(string language)
        {
            return language == Spanish || language == French || language == German || language == Italian;
        }

        // The PAL disc carries official UK, French, German, Italian and Spanish
        // banks. The game picks one from the console language (XC_LANGUAGE), so
        // nothing is installed. Others fall back to the official English text.
        internal static int ConsoleLanguage(string language)
        {
            switch (language)
            {
                case German: return 3;
                case French: return 4;
                case Spanish: return 5;
                case Italian: return 6;
                default: return 1;
            }
        }

        internal static string DisplayName(string language, string release)
        {
            if (release != GameRelease.Pal) return DisplayName(language);
            switch (language)
            {
                case PortugueseBrazil: return "Português (Brasil) - English game text on PAL";
                case Spanish: return "Español (official)";
                case French: return "Français (official)";
                case German: return "Deutsch (official)";
                case Italian: return "Italiano (official)";
                default: return "English (official UK)";
            }
        }

        internal static string DisplayName(string language)
        {
            switch (language)
            {
                case English: return "English";
                case PortugueseBrazil: return "Português (Brasil)";
                case Spanish: return "Spanish (machine draft)";
                case French: return "French (machine draft)";
                case German: return "German (machine draft)";
                case Italian: return "Italian (machine draft)";
                default: return "English";
            }
        }

        internal static string BankPath(string root, string language)
        {
            if (!IsSupported(language)) throw new ArgumentException("Choose one of the supported game languages.", "language");
            return Path.Combine(Path.GetFullPath(root), "local", "language-banks", language, "MainUS.bin");
        }

        internal static string GamePath(string root)
        {
            return Path.Combine(Path.GetFullPath(root), "game", "language", "strings", "MainUS.bin");
        }

        internal static void Install(string root, string language)
        {
            Install(root, language, StockEnglishSha256);
        }

        internal static void Install(string root, string language, string expectedEnglishSha256)
        {
            string source = BankPath(root, language);
            string target = GamePath(root);
            if (language != English && !File.Exists(source))
                throw new FileNotFoundException("The selected language bank is missing. Restore it under local\\language-banks\\" + language + "\\MainUS.bin.", source);
            if (!File.Exists(target))
                throw new FileNotFoundException("The game's MainUS.bin is missing from game\\language\\strings.", target);

            bool preserved = PreserveEnglishBank(root, target, expectedEnglishSha256);
            if (language == English && preserved) return;
            string temporary = TemporaryPath(target);
            try
            {
                CopyBank(source, temporary);
                if (BankHash(temporary) == BankHash(target)) return;
                File.Replace(temporary, target, null);
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }

        private static bool PreserveEnglishBank(string root, string target, string expectedSha256)
        {
            string english = BankPath(root, English);
            if (File.Exists(english)) return false;

            Directory.CreateDirectory(Path.GetDirectoryName(english));
            string temporary = TemporaryPath(english);
            try
            {
                CopyBank(target, temporary);
                string actualSha256 = BankHash(temporary);
                if (!String.Equals(actualSha256, expectedSha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("The game's MainUS.bin is not the recognized original English bank. Restore the original English MainUS.bin from your supported retail game files under local\\language-banks\\en-US\\MainUS.bin before changing languages.");

                // Rename within this directory publishes a complete fallback without
                // overwriting an English bank another launcher may have preserved.
                try { File.Move(temporary, english); return true; }
                catch (IOException) { if (!File.Exists(english)) throw; return false; }
            }
            finally
            {
                if (File.Exists(temporary)) File.Delete(temporary);
            }
        }

        private static string TemporaryPath(string target)
        {
            return target + ".launcher-tmp-" + Guid.NewGuid().ToString("N");
        }

        private static string BankHash(string path)
        {
            using (var hash = SHA256.Create())
            using (var stream = File.OpenRead(path))
                return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", "");
        }

        private static void CopyBank(string source, string temporary)
        {
            using (var input = File.OpenRead(source))
            using (var output = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write))
                input.CopyTo(output);
        }
    }
}
