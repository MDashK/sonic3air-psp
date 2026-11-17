using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace S3AirPspModConverter
{
	static class Program
	{
		[STAThread]
		static int Main(string[] args)
		{
			if (args.Length > 0 && args[0] == "--cli")
				return RunCommandLine(args);
			if (args.Length >= 3 && args[0] == "--pack-sprites")
				return PackSprites(args[1], args[2]);

			Application.EnableVisualStyles();
			Application.SetCompatibleTextRenderingDefault(false);
			Application.Run(new MainForm());
			return 0;
		}

		public static string AtracdencPath => Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "atracdenc.exe");

		// The game's own sprites for the PSP (used by psp/package.sh):
		//   S3AIR-PSP-ModConverter.exe --pack-sprites <data folder, with "sprites" in it> <output file, data/sprites_psp.bin>
		private static int PackSprites(string dataFolder, string output)
		{
			try
			{
				SpritePack pack = SpritePack.Build(dataFolder);
				pack.Write(output);
				Console.WriteLine("Sprite pack: " + pack.Sprites.Count + " sprites, " + Mods.FormatKB(pack.PixelBytes) + " of pixels, " + pack.Palettes.Count + " palettes");
				foreach (string warning in pack.Warnings)
					Console.WriteLine("  " + warning);
				return 0;
			}
			catch (Exception e)
			{
				Console.WriteLine("Sprite pack failed: " + e.Message);
				return 1;
			}
		}

		// Without the window (for tests and scripts):
		//   S3AIR-PSP-ModConverter.exe --cli <mods folder> <output folder | -> <report file> [mod name filter]
		// "-" as output only analyzes. The report lists each mod with its memory need and notes.
		private static int RunCommandLine(string[] args)
		{
			if (args.Length < 4)
				return 2;
			string source = args[1], output = args[2], reportPath = args[3];
			string filter = args.Length > 4 ? args[4] : null;
			bool exactName = filter != null && filter.StartsWith("=");
			if (exactName)
				filter = filter.Substring(1);
			var report = new StringBuilder();
			Action<string> log = line =>
			{
				lock (report)
				{
					report.AppendLine(line);
					File.WriteAllText(reportPath, report.ToString());
				}
			};
			int failures = 0;
			foreach (ModInfo mod in Mods.FindMods(source, log))
			{
				if (filter != null && (exactName ? !mod.SourceName.Equals(filter, StringComparison.OrdinalIgnoreCase) : mod.SourceName.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0))
					continue;
				var start = DateTime.Now;
				Mods.Analyze(mod);
				log("== " + mod.SourceName + " | " + mod.DisplayName + " | " + mod.ContentSummary + " | budget " + Mods.FormatKB(mod.BudgetBytes) +
					(mod.Sprites != null ? " (always " + Mods.FormatKB(mod.MemoryBytes) + ": sprite objects " + Mods.FormatKB(mod.Sprites.ResidentBytes) + ", sounds " + Mods.FormatKB(mod.SoundBytesEstimate) +
						", scripts " + Mods.FormatKB(mod.ScriptSourceBytes * 2) + ", data " + Mods.FormatKB(mod.OtherDataBytes) + "; sprite pixels " + Mods.FormatKB(mod.SpritePixelBytes) + " when used)" : "") +
					(mod.FitsBudget ? "" : " | OVER BUDGET"));
				if (mod.Error != null)
				{
					log("   ERROR: " + mod.Error);
					++failures;
					continue;
				}
				foreach (string warning in mod.Warnings.Take(12))
					log("   warning: " + warning);
				if (mod.Warnings.Count > 12)
					log("   (" + (mod.Warnings.Count - 12) + " more warnings)");
				if (output != "-")
				{
					try
					{
						Mods.Convert(mod, output, AtracdencPath, log, p => { }, CancellationToken.None);
						log("   converted in " + (DateTime.Now - start).TotalSeconds.ToString("0.0") + " s, final budget " + Mods.FormatKB(mod.BudgetBytes) + " (always " + Mods.FormatKB(mod.MemoryBytes) + ")");
					}
					catch (Exception e)
					{
						log("   CONVERSION FAILED: " + e);
						++failures;
					}
				}
			}
			log("done");
			return failures > 0 ? 1 : 0;
		}
	}
}
