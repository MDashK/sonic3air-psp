using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace S3AirPspModConverter
{
	// Makes "sprites/sprites_psp.bin": a mod's sprites already cut from their sheets, exactly as the game creates them from
	// the sprite JSON files (SpriteCollection::loadSpriteDefinitions), so the PSP reads them one by one without any sheet
	// in memory. Format: see SpriteCollection::loadSpritePack in the engine.
	sealed class SpritePack
	{
		public sealed class Sprite
		{
			public string Identifier;
			public bool IsComponent;		// RGBA (from .png) instead of 8-bit palette indices (from .bmp)
			public int Width, Height;
			public int OffsetX, OffsetY;
			public int PaletteIndex = -1;
			public byte[] Pixels;			// Width * Height bytes, or 4 times that for component sprites
		}

		public readonly List<uint[]> Palettes = new List<uint[]>();
		public readonly List<Sprite> Sprites = new List<Sprite>();
		public readonly HashSet<string> UsedFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);	// Full paths of the images and JSON files
		public readonly List<string> Warnings = new List<string>();

		// Memory on the PSP all the time: the sprite objects and their entries in the sprite collection (~192 bytes each),
		// and the palettes (registered once per sheet). The pixels only get loaded when used, see PixelBytes.
		public long ResidentBytes
		{
			get
			{
				long bytes = 0;
				foreach (Sprite sprite in Sprites)
					bytes += 192;
				foreach (uint[] palette in Palettes)
					bytes += palette.Length * 4 + 96;
				return bytes;
			}
		}

		public long PixelBytes
		{
			get
			{
				long bytes = 0;
				foreach (Sprite sprite in Sprites)
					bytes += sprite.Pixels.Length;
				return bytes;
			}
		}

		// The sprite JSON files of a mod's "sprites" folder, in the order the game finds them
		public static List<string> FindSpriteJsonFiles(string spritesFolder)
		{
			var result = new List<string>();
			if (Directory.Exists(spritesFolder))
				CollectJsonFiles(spritesFolder, result);
			return result;
		}

		private static void CollectJsonFiles(string folder, List<string> result)
		{
			var files = new List<string>(Directory.GetFiles(folder, "*.json"));
			files.Sort(StringComparer.OrdinalIgnoreCase);
			result.AddRange(files);
			var subfolders = new List<string>(Directory.GetDirectories(folder));
			subfolders.Sort(StringComparer.OrdinalIgnoreCase);
			foreach (string subfolder in subfolders)
				CollectJsonFiles(subfolder, result);
		}

		public static SpritePack Build(string modFolder)
		{
			var pack = new SpritePack();
			var paletteSheets = new Dictionary<string, PaletteImage>(StringComparer.Ordinal);		// By full path as the game builds it (its sheet key)
			var paletteSheetIndex = new Dictionary<string, int>(StringComparer.Ordinal);
			var componentSheets = new Dictionary<string, RgbaImage>(StringComparer.Ordinal);
			var failedSheets = new HashSet<string>(StringComparer.Ordinal);

			foreach (string jsonPath in FindSpriteJsonFiles(Path.Combine(modFolder, "sprites")))
			{
				pack.UsedFiles.Add(jsonPath);
				JsonNode root;
				try
				{
					root = Json.Load(jsonPath);
				}
				catch (Exception e)
				{
					pack.Warnings.Add("Sprite file skipped (the game would skip it too): " + e.Message);
					continue;
				}
				if (!(root is JsonObject rootObject))
					continue;

				string folder = Path.GetDirectoryName(jsonPath) + Path.DirectorySeparatorChar;
				foreach (var member in rootObject.SortedMembers())
				{
					if (!(member.Value is JsonObject definition))
						continue;

					// As in the game: "File", "Center" ("x,y") and "Rect" ("x,y,width,height"), only as strings
					string filename = (definition.Get("File") as JsonString)?.Value ?? "";
					int[] center = ParseInts(definition.Get("Center"), 2) ?? new int[2];
					int[] rect = ParseInts(definition.Get("Rect"), 4) ?? new int[4];
					if (filename.Length == 0)
						continue;

					string fullPath = folder + filename;
					var sprite = new Sprite { Identifier = member.Key, IsComponent = filename.EndsWith(".png", StringComparison.Ordinal) };
					bool isSheet = (rect[2] != 0);
					string sheetFile = Path.GetFullPath(fullPath.Replace('/', Path.DirectorySeparatorChar));
					pack.UsedFiles.Add(sheetFile);

					if (!sprite.IsComponent)
					{
						if (isSheet)
						{
							if (!paletteSheets.TryGetValue(fullPath, out PaletteImage sheet) && !failedSheets.Contains(fullPath))
							{
								sheet = LoadBmp(sheetFile, out string error);
								if (sheet != null)
								{
									paletteSheets[fullPath] = sheet;
									paletteSheetIndex[fullPath] = pack.Palettes.Count;
									pack.Palettes.Add(sheet.Palette);
								}
								else
								{
									failedSheets.Add(fullPath);
									pack.Warnings.Add("Sprite sheet '" + filename + "' can't be loaded (" + error + "): its sprites stay empty, as in the game");
								}
							}
							if (sheet != null)
							{
								CutPaletteSprite(sprite, sheet, rect);
								sprite.PaletteIndex = paletteSheetIndex[fullPath];
								sprite.OffsetX = -center[0];
								sprite.OffsetY = -center[1];
							}
						}
						else
						{
							PaletteImage image = LoadBmp(sheetFile, out string error);
							if (image != null)
							{
								CutPaletteSprite(sprite, image, new[] { 0, 0, image.Width, image.Height });
								sprite.PaletteIndex = pack.Palettes.Count;
								pack.Palettes.Add(image.Palette);
								sprite.OffsetX = -center[0];
								sprite.OffsetY = -center[1];
							}
							else
							{
								pack.Warnings.Add("Sprite image '" + filename + "' can't be loaded (" + error + "): the sprite stays empty, as in the game");
							}
						}
					}
					else
					{
						RgbaImage image = null;
						if (isSheet)
						{
							if (!componentSheets.TryGetValue(fullPath, out image) && !failedSheets.Contains(fullPath))
							{
								image = LoadPng(sheetFile, out string error);
								if (image != null)
									componentSheets[fullPath] = image;
								else
								{
									failedSheets.Add(fullPath);
									pack.Warnings.Add("Sprite sheet '" + filename + "' can't be loaded (" + error + "): its sprites stay empty, as in the game");
								}
							}
						}
						else
						{
							image = LoadPng(sheetFile, out string error);
							if (image == null)
								pack.Warnings.Add("Sprite image '" + filename + "' can't be loaded (" + error + "): the sprite stays empty, as in the game");
						}
						if (image != null)
							CutComponentSprite(sprite, image, isSheet ? rect : new[] { 0, 0, image.Width, image.Height });
						sprite.OffsetX = -center[0];
						sprite.OffsetY = -center[1];
					}

					if (sprite.Pixels == null)
						sprite.Pixels = new byte[0];
					if (sprite.Width > 512 || sprite.Height > 512)
						pack.Warnings.Add("Sprite '" + sprite.Identifier + "' is " + sprite.Width + "x" + sprite.Height + " pixels: the PSP can't draw sprites larger than 512x512");
					pack.Sprites.Add(sprite);
				}
			}
			return pack;
		}

		public void Write(string path)
		{
			using (var stream = new FileStream(path, FileMode.Create, FileAccess.Write))
			using (var writer = new BinaryWriter(stream))
			{
				writer.Write(Encoding.ASCII.GetBytes("S3SP"));
				writer.Write(1u);
				writer.Write((uint)Palettes.Count);
				writer.Write((uint)Sprites.Count);
				foreach (uint[] palette in Palettes)
				{
					writer.Write((uint)palette.Length);
					foreach (uint color in palette)
						writer.Write(color);
				}
				foreach (Sprite sprite in Sprites)
				{
					byte[] identifier = Encoding.UTF8.GetBytes(sprite.Identifier);
					writer.Write((ushort)identifier.Length);
					writer.Write(identifier);
					writer.Write((byte)(sprite.IsComponent ? 1 : 0));
					writer.Write((byte)0);
					writer.Write((short)sprite.Width);
					writer.Write((short)sprite.Height);
					writer.Write((short)sprite.OffsetX);
					writer.Write((short)sprite.OffsetY);
					writer.Write(sprite.PaletteIndex);
					writer.Write(sprite.Pixels);
				}
			}
		}

		// "x,y" or "x,y,width,height" as the game parses it (String::parseInt on each part)
		private static int[] ParseInts(JsonNode node, int count)
		{
			if (!(node is JsonString text) || text.Value.Length == 0)
				return null;
			string[] parts = text.Value.Split(',');
			if (parts.Length != count)
				return null;
			var result = new int[count];
			for (int i = 0; i < count; ++i)
				result[i] = ParseInt(parts[i]);
			return result;
		}

		private static int ParseInt(string text)
		{
			text = text.Trim();
			bool negative = text.StartsWith("-");
			if (negative || text.StartsWith("+"))
				text = text.Substring(1).TrimStart();
			long value = 0;
			if (text.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
			{
				foreach (char c in text.Substring(2))
				{
					int digit = Uri.IsHexDigit(c) ? Convert.ToInt32(c.ToString(), 16) : -1;
					if (digit < 0)
						break;
					value = value * 16 + digit;
				}
			}
			else
			{
				foreach (char c in text)
				{
					if (c < '0' || c > '9')
						break;
					value = value * 10 + (c - '0');
				}
			}
			return (int)(negative ? -value : value);
		}

		private static int RoundUp4(int value) => (value + 3) & ~3;

		// PaletteSprite::createFromBitmap: size rounded up to a multiple of 4, the rectangle's part inside the sheet copied
		// to the top left corner (the rest stays 0, i.e. transparent)
		private static void CutPaletteSprite(Sprite sprite, PaletteImage sheet, int[] rect)
		{
			int width = RoundUp4(rect[2]);
			int height = RoundUp4(rect[3]);
			if (width <= 0 || height <= 0)
			{
				sprite.Pixels = new byte[0];
				return;
			}
			sprite.Width = width;
			sprite.Height = height;
			sprite.Pixels = new byte[width * height];

			int px = rect[0], py = rect[1], sx = rect[2], sy = rect[3];
			if (px < 0) { sx += px; px = 0; }
			if (py < 0) { sy += py; py = 0; }
			if (px + sx > sheet.Width) sx = sheet.Width - px;
			if (py + sy > sheet.Height) sy = sheet.Height - py;
			if (sx <= 0 || sy <= 0)
				return;
			for (int y = 0; y < sy; ++y)
				Buffer.BlockCopy(sheet.Pixels, (py + y) * sheet.Width + px, sprite.Pixels, y * width, sx);
		}

		// Bitmap::copy with a rectangle: just the part inside the sheet, no rounding
		private static void CutComponentSprite(Sprite sprite, RgbaImage sheet, int[] rect)
		{
			int px = rect[0], py = rect[1], sx = rect[2], sy = rect[3];
			if (px < 0) { sx += px; px = 0; }
			if (py < 0) { sy += py; py = 0; }
			if (px + sx > sheet.Width) sx = sheet.Width - px;
			if (py + sy > sheet.Height) sy = sheet.Height - py;
			if (sx <= 0 || sy <= 0)
			{
				sprite.Pixels = new byte[0];
				return;
			}
			sprite.Width = sx;
			sprite.Height = sy;
			sprite.Pixels = new byte[sx * sy * 4];
			for (int y = 0; y < sy; ++y)
				Buffer.BlockCopy(sheet.Pixels, ((py + y) * sheet.Width + px) * 4, sprite.Pixels, y * sx * 4, sx * 4);
		}

		public sealed class PaletteImage
		{
			public int Width, Height;
			public byte[] Pixels;
			public uint[] Palette;		// As the game has them: 0xAABBGGRR, all opaque
		}

		public sealed class RgbaImage
		{
			public int Width, Height;
			public byte[] Pixels;		// R, G, B, A per pixel
		}

		// The same as PaletteBitmap::loadBMP in the game, quirks included (rows always bottom-up, 1-bit pixels LSB first)
		public static PaletteImage LoadBmp(string path, out string error)
		{
			error = null;
			byte[] content;
			try
			{
				content = File.ReadAllBytes(path);
			}
			catch (Exception)
			{
				error = "file not found";
				return null;
			}
			if (content.Length < 54 || content[0] != 'B' || content[1] != 'M')
			{
				error = "not a BMP file";
				return null;
			}
			uint headerSize = BitConverter.ToUInt32(content, 10);
			uint dibHeaderSize = BitConverter.ToUInt32(content, 14);
			int width = BitConverter.ToInt32(content, 18);
			int height = BitConverter.ToInt32(content, 22);
			int bitdepth = BitConverter.ToUInt16(content, 28);
			uint compression = BitConverter.ToUInt32(content, 30);
			uint numColors = BitConverter.ToUInt32(content, 46);
			int stride = (width * bitdepth + 31) / 32 * 4;
			long position = 54;
			if (dibHeaderSize > 0x28)
				position += dibHeaderSize - 0x28;

			int palSize = 0;
			if (bitdepth == 1 || bitdepth == 4 || bitdepth == 8)
				palSize = (numColors != 0) ? (int)numColors : (1 << bitdepth);
			if (palSize == 0)
			{
				error = "not a palette BMP (" + bitdepth + " bits per pixel): sprites in .bmp files need 8-bit palette images";
				return null;
			}
			if (palSize > 256)
			{
				error = "palette with " + palSize + " colors";
				return null;
			}
			if (width <= 0 || height <= 0)
			{
				error = "unsupported size " + width + "x" + height + " (top-down BMP files aren't supported by the game)";
				return null;
			}
			if (compression != 0)
			{
				error = "compressed BMP files aren't supported by the game";
				return null;
			}
			if (position + palSize * 4 > content.Length)
			{
				error = "file too short";
				return null;
			}

			var palette = new uint[palSize];
			for (int i = 0; i < palSize; ++i)
			{
				uint color = BitConverter.ToUInt32(content, (int)position + i * 4) | 0xff000000;
				palette[i] = (color & 0xff00ff00) | ((color & 0xff) << 16) | ((color >> 16) & 0xff);
			}
			position += palSize * 4;
			if (headerSize > position)
				position = headerSize;

			long expectedSize = (long)((width * bitdepth + 7) / 8) * height;
			if (content.Length - position < expectedSize)
			{
				error = "file too short";
				return null;
			}

			var pixels = new byte[width * height];
			long rowStart = position;
			for (int y = 0; y < height; ++y)
			{
				int destination = (height - y - 1) * width;
				for (int x = 0; x < width; ++x)
				{
					long index;
					switch (bitdepth)
					{
						case 1:
							index = rowStart + x / 8;
							pixels[destination + x] = (index < content.Length) ? (byte)((content[index] >> (x & 7)) & 1) : (byte)0;
							break;
						case 4:
							index = rowStart + x / 2;
							pixels[destination + x] = (index < content.Length) ? (byte)((content[index] >> (4 - (x & 1) * 4)) & 0x0f) : (byte)0;
							break;
						default:
							index = rowStart + x;
							pixels[destination + x] = (index < content.Length) ? content[index] : (byte)0;
							break;
					}
				}
				rowStart += stride;
			}
			return new PaletteImage { Width = width, Height = height, Pixels = pixels, Palette = palette };
		}

		// PNG as RGBA. Color correction chunks are removed first: Windows would apply them, the game doesn't.
		public static RgbaImage LoadPng(string path, out string error)
		{
			error = null;
			byte[] content;
			try
			{
				content = File.ReadAllBytes(path);
			}
			catch (Exception)
			{
				error = "file not found";
				return null;
			}
			try
			{
				using (var stream = new MemoryStream(StripColorCorrection(content)))
				using (var image = Image.FromStream(stream, false, true))
				{
					// (LockBits converts any pixel format to 32-bit ARGB, without drawing, i.e. no scaling or blending)
					var bitmap = (Bitmap)image;
					int width = bitmap.Width, height = bitmap.Height;
					var data = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
					var pixels = new byte[width * height * 4];
					for (int y = 0; y < height; ++y)
						Marshal.Copy(data.Scan0 + y * data.Stride, pixels, y * width * 4, width * 4);
					bitmap.UnlockBits(data);
					for (int i = 0; i < pixels.Length; i += 4)
					{
						byte b = pixels[i];
						pixels[i] = pixels[i + 2];
						pixels[i + 2] = b;
					}
					return new RgbaImage { Width = width, Height = height, Pixels = pixels };
				}
			}
			catch (Exception e)
			{
				error = "not a valid PNG file: " + e.Message;
				return null;
			}
		}

		private static byte[] StripColorCorrection(byte[] png)
		{
			if (png.Length < 8 || png[1] != 'P' || png[2] != 'N' || png[3] != 'G')
				return png;
			var output = new MemoryStream();
			output.Write(png, 0, 8);
			int position = 8;
			while (position + 12 <= png.Length)
			{
				int length = (png[position] << 24) | (png[position + 1] << 16) | (png[position + 2] << 8) | png[position + 3];
				if (length < 0 || position + 12 + length > png.Length)
					break;
				string type = Encoding.ASCII.GetString(png, position + 4, 4);
				if (type != "gAMA" && type != "cHRM" && type != "sRGB" && type != "iCCP")
					output.Write(png, position, 12 + length);
				position += 12 + length;
			}
			return output.ToArray();
		}
	}
}
