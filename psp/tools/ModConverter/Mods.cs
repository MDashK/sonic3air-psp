using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace S3AirPspModConverter
{
	// A mod found in the source folder, and what converting it means for the PSP
	sealed class ModInfo
	{
		public string SourceFolder;		// The folder with "mod.json" (for a zip: where it got extracted to)
		public string SourceName;		// As shown: the folder or zip file name
		public string DirectoryName;	// The mod's folder name in the output
		public string DisplayName = "";
		public string Author = "";
		public string ModVersion = "";

		public bool HasScripts;
		public long ScriptSourceBytes;
		public SpritePack Sprites;
		public int NumMusic, NumJingles, NumSounds, NumDroppedAudio;
		public long SoundBytesEstimate;
		public long OtherDataBytes;
		public long MemoryBytes;			// Needed all the time while the mod is active ("MemoryBytes" in psp.json)
		public long OutputBytesEstimate;

		public long SpritePixelBytes => Sprites?.PixelBytes ?? 0;
		// What the mod takes of the PSP's mod memory budget: what it needs all the time, plus the sprite cache it needs
		// at least (its sprites' pixels get loaded when used) -- the same rule as ModManager::fitsPspMemoryBudget
		public long BudgetBytes => MemoryBytes + Math.Min(SpritePixelBytes, Mods.SpriteCacheMin);

		public readonly List<string> Notes = new List<string>();		// Shown to the user, not a problem
		public readonly List<string> Warnings = new List<string>();		// Something won't work as on a PC
		public string Error;											// Can't be converted at all

		public bool FitsBudget => BudgetBytes <= Mods.MemoryBudget;

		public string ContentSummary
		{
			get
			{
				var parts = new List<string>();
				if (HasScripts)
					parts.Add("scripts");
				if (Sprites != null && Sprites.Sprites.Count > 0)
					parts.Add(Sprites.Sprites.Count + " sprites");
				if (NumMusic > 0)
					parts.Add(NumMusic + " music");
				if (NumJingles > 0)
					parts.Add(NumJingles + " jingles");
				if (NumSounds > 0)
					parts.Add(NumSounds + " sounds");
				if (OtherDataBytes > 0)
					parts.Add("data");
				return parts.Count > 0 ? string.Join(", ", parts) : "nothing";
			}
		}
	}

	static class Mods
	{
		// Must be the same as ModManager::PSP_MOD_MEMORY_BUDGET and PSP_MOD_SPRITE_CACHE_MIN in the engine
		public const long MemoryBudget = 1536 * 1024;
		public const long SpriteCacheMin = 512 * 1024;
		public const string ConverterVersion = "1.0";

		// What the game reads from a mod's folder (besides mod.json and the icons): all else stays on the PC
		private static readonly string[] CopiedFolders = { "scripts", "palettes", "rawdata", "font" };
		// Of these, what's in memory while the mod is active (scripts are counted separately)
		private static readonly string[] DataFolders = { "palettes", "rawdata", "font" };

		private static readonly HashSet<string> JunkNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
			{ "Thumbs.db", "desktop.ini", ".DS_Store", "psp.json" };
		private static readonly HashSet<string> JunkFolders = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
			{ "__MACOSX", ".git", ".svn", ".vs" };
		private static readonly HashSet<string> JunkExtensions = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
			{ ".psd", ".ase", ".aseprite", ".xcf", ".kra", ".pdn", ".zip", ".rar", ".7z", ".bak", ".tmp", ".wav.asd", ".flp", ".mmpz" };
		private static readonly HashSet<string> DocumentExtensions = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
			{ ".txt", ".md", ".pdf", ".url", ".html", ".htm", ".rtf", ".docx" };

		public static string TempRoot => Path.Combine(Path.GetTempPath(), "S3AIR-PSP-ModConverter");

		// Mods in a folder: subfolders with "mod.json" (also deeper down, like the game does) and .zip files with a mod
		public static List<ModInfo> FindMods(string folder, Action<string> log)
		{
			var result = new List<ModInfo>();
			if (File.Exists(Path.Combine(folder, "mod.json")))
			{
				result.Add(new ModInfo { SourceFolder = folder, SourceName = Path.GetFileName(folder), DirectoryName = Path.GetFileName(folder) });
				return result;
			}
			FindModsRecursive(folder, "", result, log, 0);
			return result;
		}

		private static void FindModsRecursive(string folder, string prefix, List<ModInfo> result, Action<string> log, int depth)
		{
			var subfolders = Directory.GetDirectories(folder).OrderBy(f => f, StringComparer.OrdinalIgnoreCase);
			foreach (string subfolder in subfolders)
			{
				string name = Path.GetFileName(subfolder);
				if (JunkFolders.Contains(name))
					continue;
				if (File.Exists(Path.Combine(subfolder, "mod.json")))
					result.Add(new ModInfo { SourceFolder = subfolder, SourceName = prefix + name, DirectoryName = name });
				else if (depth < 3)
					FindModsRecursive(subfolder, prefix + name + "/", result, log, depth + 1);
			}
			foreach (string zip in Directory.GetFiles(folder, "*.zip").OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
			{
				try
				{
					result.AddRange(ExtractZipMods(zip, prefix));
				}
				catch (Exception e)
				{
					log?.Invoke("Can't read " + Path.GetFileName(zip) + ": " + e.Message);
				}
			}
		}

		private static List<ModInfo> ExtractZipMods(string zipPath, string prefix)
		{
			var result = new List<ModInfo>();
			string target = Path.Combine(TempRoot, "zips", Path.GetFileNameWithoutExtension(zipPath) + "_" + Math.Abs(zipPath.GetHashCode()).ToString("x"));
			if (Directory.Exists(target))
				Directory.Delete(target, true);
			Directory.CreateDirectory(target);
			string fullTarget = Path.GetFullPath(target) + Path.DirectorySeparatorChar;
			using (ZipArchive archive = ZipFile.OpenRead(zipPath))
			{
				foreach (ZipArchiveEntry entry in archive.Entries)
				{
					string destination = Path.GetFullPath(Path.Combine(target, entry.FullName));
					if (!destination.StartsWith(fullTarget, StringComparison.OrdinalIgnoreCase))
						continue;		// (no paths out of the folder)
					if (entry.FullName.EndsWith("/") || entry.FullName.EndsWith("\\"))
					{
						Directory.CreateDirectory(destination);
						continue;
					}
					Directory.CreateDirectory(Path.GetDirectoryName(destination));
					entry.ExtractToFile(destination, true);
				}
			}
			string zipName = Path.GetFileName(zipPath);
			if (File.Exists(Path.Combine(target, "mod.json")))
			{
				result.Add(new ModInfo { SourceFolder = target, SourceName = prefix + zipName, DirectoryName = Path.GetFileNameWithoutExtension(zipPath) });
				return result;
			}
			foreach (string modJson in Directory.GetFiles(target, "mod.json", SearchOption.AllDirectories).OrderBy(f => f, StringComparer.OrdinalIgnoreCase))
			{
				string folder = Path.GetDirectoryName(modJson);
				if (folder.Split(Path.DirectorySeparatorChar).Any(part => JunkFolders.Contains(part)))
					continue;
				result.Add(new ModInfo { SourceFolder = folder, SourceName = prefix + zipName + "/" + Path.GetFileName(folder), DirectoryName = Path.GetFileName(folder) });
			}
			return result;
		}

		private static bool IsJunk(string relativePath)
		{
			string[] parts = relativePath.Split(Path.DirectorySeparatorChar);
			if (parts.Take(parts.Length - 1).Any(part => JunkFolders.Contains(part)))
				return true;
			string name = parts[parts.Length - 1];
			return JunkNames.Contains(name) || name.StartsWith("._") || JunkExtensions.Contains(Path.GetExtension(name));
		}

		private static string FirstFolder(string relativePath)
		{
			int slash = relativePath.IndexOf(Path.DirectorySeparatorChar);
			return slash < 0 ? "" : relativePath.Substring(0, slash);
		}

		private static IEnumerable<string> AllFiles(string folder)
		{
			return Directory.GetFiles(folder, "*", SearchOption.AllDirectories)
				.Select(path => path.Substring(folder.Length).TrimStart(Path.DirectorySeparatorChar));
		}

		// Everything the PSP needs to know, without writing anything
		public static void Analyze(ModInfo mod)
		{
			try
			{
				AnalyzeInternal(mod);
			}
			catch (Exception e)
			{
				mod.Error = e.Message;
			}
		}

		private static void AnalyzeInternal(ModInfo mod)
		{
			// mod.json
			JsonObject modJson = Json.Load(Path.Combine(mod.SourceFolder, "mod.json")) as JsonObject;
			if (modJson == null)
				throw new InvalidDataException("mod.json is not a JSON object");
			if (modJson.Get("Metadata") is JsonObject metadata)
			{
				mod.DisplayName = metadata.GetString("Name") ?? "";
				mod.Author = metadata.GetString("Author") ?? "";
				mod.ModVersion = metadata.GetString("ModVersion") ?? "";
			}
			if (mod.DisplayName.Length == 0)
				mod.DisplayName = mod.DirectoryName;

			// Scripts: compiled by the PSP itself when the mod gets loaded
			string scripts = Path.Combine(mod.SourceFolder, "scripts");
			if (File.Exists(Path.Combine(scripts, "main.lemon")))
			{
				mod.HasScripts = true;
				mod.ScriptSourceBytes = Directory.GetFiles(scripts, "*.lemon", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length);
				mod.Notes.Add("Scripts get compiled by the PSP when the mod is loaded: that takes a moment, and script mods are less tested on the PSP");
			}
			string text = File.Exists(Path.Combine(scripts, "main.lemon")) ? ReadAllScripts(scripts) : "";
			if (text.IndexOf("getNumPlayers", StringComparison.OrdinalIgnoreCase) >= 0 || mod.DisplayName.IndexOf("co-op", StringComparison.OrdinalIgnoreCase) >= 0)
				mod.Warnings.Add("Looks like a multiplayer mod: the PSP has only one player");

			// Sprites
			mod.Sprites = SpritePack.Build(mod.SourceFolder);
			mod.Warnings.AddRange(mod.Sprites.Warnings);

			// Audio
			AnalyzeAudio(mod);

			// Other data the game reads (palettes, ROM injections, fonts): counted as memory, images as their decoded size
			long otherBytes = 0;
			foreach (string relative in AllFiles(mod.SourceFolder))
			{
				if (IsJunk(relative) || !DataFolders.Contains(FirstFolder(relative), StringComparer.OrdinalIgnoreCase))
					continue;
				otherBytes += DataMemoryBytes(Path.Combine(mod.SourceFolder, relative));
			}
			mod.OtherDataBytes = otherBytes;

			// Size on the memory stick: music (estimated in AnalyzeAudio), the sprite pack, sounds, and what gets copied
			mod.OutputBytesEstimate += mod.SpritePixelBytes + mod.Sprites.Sprites.Count * 40 + mod.SoundBytesEstimate;
			foreach (string relative in AllFiles(mod.SourceFolder))
			{
				if (!IsJunk(relative) && CopiedFolders.Contains(FirstFolder(relative), StringComparer.OrdinalIgnoreCase))
					mod.OutputBytesEstimate += new FileInfo(Path.Combine(mod.SourceFolder, relative)).Length;
			}

			UpdateMemory(mod, mod.SoundBytesEstimate);
			if (!mod.FitsBudget)
				mod.Warnings.Insert(0, "Needs " + FormatKB(mod.BudgetBytes) + " of memory: more than the PSP has for all mods together (" + FormatKB(MemoryBudget) + ")");
			if (mod.SpritePixelBytes > 0)
				mod.Notes.Add("Sprites: " + FormatKB(mod.SpritePixelBytes) + " of pixels, loaded when they're used (a cache of at least " + FormatKB(SpriteCacheMin) + " on the PSP)");
		}

		// What the mod needs in RAM all the time while it's active ("MemoryBytes" in psp.json, checked by the game):
		//  - Sprites: their objects and palettes (the pixels are loaded when used, into the sprite cache)
		//  - Sounds: all of them, a sound stays loaded while it's used (music and jingles are streamed, almost nothing)
		//  - Scripts: the compiled code is ~2 bytes per byte of source (as measured with the game's own scripts)
		//  - Other data, the audio definitions, the mod itself
		private static void UpdateMemory(ModInfo mod, long soundBytes)
		{
			int audioEntries = mod.NumMusic + mod.NumJingles + mod.NumSounds;
			mod.MemoryBytes = mod.Sprites.ResidentBytes + soundBytes + mod.ScriptSourceBytes * 2 + mod.OtherDataBytes + audioEntries * 256 + 8 * 1024;
		}

		private static long DataMemoryBytes(string path)
		{
			string extension = Path.GetExtension(path).ToLowerInvariant();
			long size = new FileInfo(path).Length;
			try
			{
				if (extension == ".png" || extension == ".bmp")
				{
					using (var stream = File.OpenRead(path))
					{
						var header = new byte[26];
						int read = stream.Read(header, 0, header.Length);
						if (extension == ".png" && read >= 24)
							return (long)((header[16] << 24) | (header[17] << 16) | (header[18] << 8) | header[19]) * ((header[20] << 24) | (header[21] << 16) | (header[22] << 8) | header[23]) * 4;
						if (extension == ".bmp" && read >= 26)
							return Math.Abs((long)BitConverter.ToInt32(header, 18) * BitConverter.ToInt32(header, 22)) * 4;
					}
				}
			}
			catch (Exception) { }
			return size;
		}

		// The Mods menu shows the icons at 64x64 and 16x16 (ModsMenu.cpp): made in that size here, so the PSP doesn't decode
		// a large image for them. Same choice of source images as the game.
		private static void WriteIcons(string source, string target)
		{
			string icon16 = Path.Combine(source, "icon-16px.png");
			string icon64 = Path.Combine(source, "icon-64px.png");
			if (!File.Exists(icon64))
				icon64 = Path.Combine(source, "icon.png");
			string large = File.Exists(icon64) ? icon64 : File.Exists(icon16) ? icon16 : null;
			string small = File.Exists(icon16) ? icon16 : File.Exists(icon64) ? icon64 : null;
			if (large != null)
				WriteScaledPng(large, Path.Combine(target, "icon-64px.png"), 64);
			if (small != null)
				WriteScaledPng(small, Path.Combine(target, "icon-16px.png"), 16);
		}

		private static void WriteScaledPng(string source, string target, int size)
		{
			try
			{
				using (var image = System.Drawing.Image.FromFile(source))
				using (var bitmap = new System.Drawing.Bitmap(size, size, System.Drawing.Imaging.PixelFormat.Format32bppArgb))
				{
					using (var graphics = System.Drawing.Graphics.FromImage(bitmap))
					{
						graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
						graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
						graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
						using (var attributes = new System.Drawing.Imaging.ImageAttributes())
						{
							attributes.SetWrapMode(System.Drawing.Drawing2D.WrapMode.TileFlipXY);		// (no dark edges)
							graphics.DrawImage(image, new System.Drawing.Rectangle(0, 0, size, size), 0, 0, image.Width, image.Height, System.Drawing.GraphicsUnit.Pixel, attributes);
						}
					}
					bitmap.Save(target, System.Drawing.Imaging.ImageFormat.Png);
				}
			}
			catch (Exception)
			{
				// (no icon then, the menu shows none)
			}
		}

		private static string ReadAllScripts(string folder)
		{
			var sb = new StringBuilder();
			foreach (string file in Directory.GetFiles(folder, "*.lemon", SearchOption.AllDirectories))
				sb.Append(File.ReadAllText(file));
			return sb.ToString();
		}

		public static string FormatMB(long bytes) => (bytes / 1048576.0).ToString(bytes < 10 * 1048576 ? "0.0" : "0", System.Globalization.CultureInfo.InvariantCulture) + " MB";

		public static string FormatKB(long bytes) => ((bytes + 1023) / 1024).ToString("N0", System.Globalization.CultureInfo.InvariantCulture) + " KB";

		// What a mod's "audio/audio_replacements.json" defines (the only audio file the game reads from a mod)
		sealed class AudioEntry
		{
			public string Key;
			public JsonObject Definition;
			public string Type;				// "Music", "Jingle" or "Sound"
			public string SourcePath;		// null: nothing to convert, keep the definition as it is
			public string Problem;			// Gets dropped, for this reason
		}

		private static List<AudioEntry> ReadAudioEntries(ModInfo mod)
		{
			var result = new List<AudioEntry>();
			string folder = Path.Combine(mod.SourceFolder, "audio");
			string jsonPath = Path.Combine(folder, "audio_replacements.json");
			if (!File.Exists(jsonPath))
			{
				if (Directory.Exists(folder) && Directory.GetFiles(folder, "*.ogg", SearchOption.AllDirectories).Length > 0)
					mod.Warnings.Add("Has audio files, but no audio/audio_replacements.json: the game doesn't use them on a PC either");
				return result;
			}
			if (!(Json.Load(jsonPath) is JsonObject root))
				return result;
			foreach (var member in root.Members)
			{
				if (!(member.Value is JsonObject definition))
					continue;
				var entry = new AudioEntry { Key = member.Key, Definition = definition, Type = definition.GetString("Type") ?? "Sound" };
				if (entry.Type != "Music" && entry.Type != "Jingle")
					entry.Type = "Sound";
				string file = definition.GetString("File") ?? "";
				string source = definition.GetString("Source") ?? "";
				if (source.Length > 0)
					entry.Problem = "uses the sound chip emulation (\"Source\"), which the PSP can't do";
				else if (file.Length > 0)
				{
					if (file.EndsWith(".chipw", StringComparison.OrdinalIgnoreCase))
						entry.Problem = "is sound chip data (.chipw), which needs the emulation the PSP can't do";
					else
					{
						entry.SourcePath = Path.GetFullPath(Path.Combine(folder, file.Replace('/', Path.DirectorySeparatorChar)));
						if (!File.Exists(entry.SourcePath))
							entry.Problem = "its file '" + file + "' is missing";
					}
				}
				result.Add(entry);
			}
			return result;
		}

		private static void AnalyzeAudio(ModInfo mod)
		{
			foreach (AudioEntry entry in ReadAudioEntries(mod))
			{
				if (entry.Problem != null)
				{
					mod.NumDroppedAudio++;
					mod.Warnings.Add("Audio '" + entry.Key + "' left out: it " + entry.Problem);
					continue;
				}
				if (entry.SourcePath == null)
					continue;
				switch (entry.Type)
				{
					case "Music": mod.NumMusic++; break;
					case "Jingle": mod.NumJingles++; break;
					default:
						mod.NumSounds++;
						try
						{
							AudioConverter.ProbeOgg(entry.SourcePath, out int rate, out int channels, out long samples);
							mod.SoundBytesEstimate += (long)(samples * (double)AudioConverter.SoundRate / rate) * Math.Min(channels, 2) * 2 + 64;
						}
						catch (Exception e)
						{
							mod.Warnings.Add("Audio '" + entry.Key + "' can't be read: " + e.Message);
						}
						break;
				}
				if (entry.Type != "Sound")
				{
					try
					{
						AudioConverter.ProbeOgg(entry.SourcePath, out int rate, out int channels, out long samples);
						mod.OutputBytesEstimate += (long)(samples / (double)rate * 16540);	// ATRAC3 132 kbps
					}
					catch (Exception e)
					{
						mod.Warnings.Add("Audio '" + entry.Key + "' can't be read: " + e.Message);
					}
				}
			}
			if (mod.NumMusic + mod.NumJingles + mod.NumSounds > 0)
				mod.Notes.Add("Audio gets converted: music and jingles to ATRAC3, sounds to 22 kHz WAV");
		}

		// Writes the PSP version of the mod into "outputFolder/<mod folder name>"
		public static void Convert(ModInfo mod, string outputFolder, string atracdenc, Action<string> log, Action<double> progress, CancellationToken cancel)
		{
			string target = Path.Combine(outputFolder, mod.DirectoryName);
			string sourceFull = Path.GetFullPath(mod.SourceFolder).TrimEnd(Path.DirectorySeparatorChar);
			string targetFull = Path.GetFullPath(target).TrimEnd(Path.DirectorySeparatorChar);
			if (string.Equals(sourceFull, targetFull, StringComparison.OrdinalIgnoreCase) ||
				targetFull.StartsWith(sourceFull + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
				sourceFull.StartsWith(targetFull + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
				throw new InvalidOperationException("The output folder can't be the mod's own folder: choose another one");
			if (Directory.Exists(target))
			{
				// Only replace what this tool made before
				if (!File.Exists(Path.Combine(target, "psp.json")))
					throw new InvalidOperationException("The output folder already has a '" + mod.DirectoryName + "' folder that this tool didn't make: move it away first");
				Directory.Delete(target, true);
			}
			Directory.CreateDirectory(target);

			// Copy what the game reads as it is: mod.json, scripts, palettes, ROM injections, fonts (sprites and audio get
			// converted below, the icons scaled); nothing else, the game never reads it
			File.Copy(Path.Combine(mod.SourceFolder, "mod.json"), Path.Combine(target, "mod.json"), true);
			foreach (string relative in AllFiles(mod.SourceFolder))
			{
				if (IsJunk(relative) || !CopiedFolders.Contains(FirstFolder(relative), StringComparer.OrdinalIgnoreCase))
					continue;
				string destination = Path.Combine(target, relative);
				Directory.CreateDirectory(Path.GetDirectoryName(destination));
				File.Copy(Path.Combine(mod.SourceFolder, relative), destination, true);
			}
			WriteIcons(mod.SourceFolder, target);
			progress(0.05);

			// Sprites
			if (mod.Sprites != null && mod.Sprites.Sprites.Count > 0)
			{
				Directory.CreateDirectory(Path.Combine(target, "sprites"));
				mod.Sprites.Write(Path.Combine(target, "sprites", "sprites_psp.bin"));
				log("  " + mod.Sprites.Sprites.Count + " sprites, " + FormatKB(mod.Sprites.PixelBytes) + " of pixels");
			}
			progress(0.1);

			// Audio
			long soundBytes = ConvertAudio(mod, Path.Combine(target, "audio"), atracdenc, log, p => progress(0.1 + 0.88 * p), cancel);

			// The final memory need, with the actual sound sizes
			UpdateMemory(mod, soundBytes);

			// What the game checks: "MemoryBytes" (needed all the time) and "SpritePixelBytes" (loaded when used); the rest
			// is for information
			var psp = new StringBuilder();
			psp.Append("{\n");
			psp.Append("\t\"Converter\": " + Json.Quote(ConverterVersion) + ",\n");
			psp.Append("\t\"MemoryBytes\": " + mod.MemoryBytes + ",\n");
			psp.Append("\t\"SpritePixelBytes\": " + mod.SpritePixelBytes + ",\n");
			psp.Append("\t\"Sprites\": " + mod.Sprites.Sprites.Count + ",\n");
			psp.Append("\t\"SpriteResidentBytes\": " + mod.Sprites.ResidentBytes + ",\n");
			psp.Append("\t\"SoundBytes\": " + soundBytes + ",\n");
			psp.Append("\t\"ScriptSourceBytes\": " + mod.ScriptSourceBytes + ",\n");
			psp.Append("\t\"OtherDataBytes\": " + mod.OtherDataBytes + "\n");
			psp.Append("}\n");
			File.WriteAllText(Path.Combine(target, "psp.json"), psp.ToString(), new UTF8Encoding(false));
			progress(1.0);
		}

		private static long ConvertAudio(ModInfo mod, string targetFolder, string atracdenc, Action<string> log, Action<double> progress, CancellationToken cancel)
		{
			List<AudioEntry> entries = ReadAudioEntries(mod);
			if (entries.Count == 0)
				return 0;
			Directory.CreateDirectory(targetFolder);
			string tempFolder = Path.Combine(TempRoot, "audio");
			Directory.CreateDirectory(tempFolder);

			// Output file names: the source's name with .at3 / .wav, unique, short enough for the audio bank
			var outputNames = new Dictionary<string, string>();		// By source path + conversion kind
			var usedNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
			var jobs = new List<Tuple<AudioEntry, string>>();
			foreach (AudioEntry entry in entries)
			{
				if (entry.Problem != null || entry.SourcePath == null)
					continue;
				string kind = entry.Type + "|" + (entry.Type == "Music" ? (entry.Definition.GetString("LoopStart") ?? "0") : "");
				string id = entry.SourcePath + "|" + kind;
				if (outputNames.ContainsKey(id))
					continue;
				string extension = (entry.Type == "Sound") ? ".wav" : ".at3";
				string baseName = new string(Path.GetFileNameWithoutExtension(entry.SourcePath).Select(c => (c < 128 && (char.IsLetterOrDigit(c) || c == '_' || c == '-')) ? c : '_').ToArray());
				if (baseName.Length > 40)
					baseName = baseName.Substring(0, 40);
				string name = baseName + extension;
				for (int n = 2; usedNames.Contains(name); ++n)
					name = baseName + "_" + n + extension;
				usedNames.Add(name);
				outputNames[id] = name;
				jobs.Add(Tuple.Create(entry, name));
			}

			// Convert (several at once: atracdenc uses one CPU core)
			var results = new Dictionary<string, long>();		// Output name -> samples (music) or bytes (sounds)
			var monoFlags = new Dictionary<string, bool>();
			var errors = new Dictionary<string, string>();
			int done = 0;
			var options = new ParallelOptions { MaxDegreeOfParallelism = Math.Max(1, Math.Min(4, Environment.ProcessorCount - 1)), CancellationToken = cancel };
			Parallel.ForEach(jobs, options, job =>
			{
				AudioEntry entry = job.Item1;
				string name = job.Item2;
				try
				{
					AudioConverter.DecodedAudio audio = AudioConverter.DecodeOgg(entry.SourcePath);
					string output = Path.Combine(targetFolder, name);
					long value;
					bool mono = false;
					if (entry.Type == "Sound")
						value = AudioConverter.ConvertToWav(audio, output, out mono);
					else
					{
						int loopStart = -1;
						if (entry.Type == "Music")
						{
							long sourceLoop = ParseLong(entry.Definition.GetString("LoopStart"));
							loopStart = (int)((sourceLoop * 44100 + audio.SampleRate / 2) / audio.SampleRate);
							if (loopStart < 0 || loopStart >= (long)audio.Length * 44100 / audio.SampleRate)
								loopStart = 0;
						}
						value = AudioConverter.ConvertToAtrac3(audio, output, loopStart, atracdenc, tempFolder);
					}
					lock (results)
					{
						results[name] = value;
						monoFlags[name] = mono;
					}
				}
				catch (Exception e)
				{
					lock (results)
						errors[name] = e.Message;
				}
				int count = Interlocked.Increment(ref done);
				lock (results)
				{
					log("  " + count + "/" + jobs.Count + ": " + Path.GetFileName(entry.SourcePath) + " -> " + name + (errors.ContainsKey(name) ? "  FAILED: " + errors[name] : ""));
					progress((double)count / jobs.Count);
				}
			});
			cancel.ThrowIfCancellationRequested();

			// The audio definitions for the PSP: the game's own format, "File" pointing to the converted files
			var json = new StringBuilder("{\n");
			var lines = new List<string>();
			long soundBytes = 0;
			var bankFiles = new List<string>();
			foreach (AudioEntry entry in entries)
			{
				if (entry.Problem != null)
					continue;
				var fields = new List<KeyValuePair<string, string>>();
				foreach (var member in entry.Definition.Members)
				{
					string value = member.Value.AsString();
					if (value == null || member.Key == "File" || member.Key == "LoopStart" || member.Key == "LoopEnd")
						continue;
					fields.Add(new KeyValuePair<string, string>(member.Key, value));
				}
				if (entry.SourcePath != null)
				{
					string kind = entry.Type + "|" + (entry.Type == "Music" ? (entry.Definition.GetString("LoopStart") ?? "0") : "");
					string name = outputNames[entry.SourcePath + "|" + kind];
					if (errors.ContainsKey(name) || !results.ContainsKey(name))
					{
						string reason = errors.ContainsKey(name) ? errors[name] : "unknown error";
						mod.Warnings.Add("Audio '" + entry.Key + "' left out: converting it failed (" + reason + ")");
						log("  Audio '" + entry.Key + "' left out: converting it failed (" + reason + ")");
						continue;
					}
					fields.Insert(0, new KeyValuePair<string, string>("File", name));
					if (entry.Type == "Music")
					{
						long sourceLoop = ParseLong(entry.Definition.GetString("LoopStart"));
						int rate;
						AudioConverter.ProbeOgg(entry.SourcePath, out rate, out _, out long sourceSamples);
						long loopStart = (sourceLoop * 44100 + rate / 2) / rate;
						if (loopStart < 0 || loopStart >= results[name])
							loopStart = 0;
						fields.Add(new KeyValuePair<string, string>("LoopStart", (loopStart + AudioConverter.Atrac3Delay).ToString()));
						fields.Add(new KeyValuePair<string, string>("LoopEnd", (results[name] + AudioConverter.Atrac3Delay).ToString()));
					}
					else if (entry.Type == "Sound")
					{
						string continuous = entry.Definition.GetString("Continuous");
						string loop = entry.Definition.GetString("LoopStart");
						if (!string.IsNullOrEmpty(continuous) && continuous != "0" && !string.IsNullOrEmpty(loop))
						{
							AudioConverter.ProbeOgg(entry.SourcePath, out int rate, out _, out _);
							fields.Add(new KeyValuePair<string, string>("LoopStart", (ParseLong(loop) * AudioConverter.SoundRate / rate).ToString()));
						}
					}
					if (!bankFiles.Contains(name))
					{
						bankFiles.Add(name);
						if (entry.Type == "Sound")
							soundBytes += results[name] + 64;
					}
				}
				lines.Add("\t" + Json.Quote(entry.Key) + ": { " + string.Join(", ", fields.Select(f => Json.Quote(f.Key) + ": " + Json.Quote(f.Value))) + " }");
			}
			json.Append(string.Join(",\n", lines)).Append("\n}\n");
			File.WriteAllText(Path.Combine(targetFolder, "audio_replacements.json"), json.ToString(), new UTF8Encoding(false));
			if (bankFiles.Count > 0)
				AudioConverter.WriteBank(targetFolder, bankFiles);
			return soundBytes;
		}

		private static long ParseLong(string text)
		{
			if (string.IsNullOrEmpty(text))
				return 0;
			text = text.Trim();
			if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
				return System.Convert.ToInt64(text.Substring(2), 16);
			return long.TryParse(text, out long value) ? value : (long)(double.TryParse(text, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out double d) ? d : 0);
		}
	}
}
