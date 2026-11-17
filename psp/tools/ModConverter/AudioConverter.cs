using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace S3AirPspModConverter
{
	// Audio for the PSP, as psp/tools/convert_audio.py does it for the game's own soundtracks:
	//  - Music and jingles as ATRAC3 (44.1 kHz stereo, decoded by the PSP's Media Engine), with padding after the end
	//  - Sound effects as 16-bit WAV, 22.05 kHz, mono where both channels are the same
	//  - Everything packed into one "audio_bank.bin" per folder (opening files on a memory stick is slow)
	static class AudioConverter
	{
		// The ATRAC3 audio as the PSP decodes it is behind the source (encoder plus decoder delay: 1162 samples measured
		// with this atracdenc, the PSP's decoder may skip some). Loop points use this a bit larger value: going back from the
		// loop end then happens inside the padding (a copy of what follows the loop start), which is seamless.
		public const int Atrac3Delay = 1280;
		// Added after the end before encoding: the encoder cuts the last partial frame, and the decoded audio is delayed
		public const int Atrac3EndPadding = 4096;
		public const int SoundRate = 22050;
		private const int BankAlignment = 2048;

		public sealed class DecodedAudio
		{
			public int SampleRate;
			public float[][] Channels;		// 1 or 2
			public int Length => Channels[0].Length;
		}

		public static DecodedAudio DecodeOgg(string path)
		{
			using (var reader = new NVorbis.VorbisReader(path))
			{
				int channels = reader.Channels;
				if (channels < 1)
					throw new InvalidDataException("no audio channels");
				var samples = new List<float[]>();
				var buffer = new float[channels * 8192];
				long total = 0;
				int count;
				while ((count = reader.ReadSamples(buffer, 0, buffer.Length)) > 0)
				{
					var chunk = new float[count];
					Array.Copy(buffer, chunk, count);
					samples.Add(chunk);
					total += count;
				}
				int frames = (int)(total / channels);
				// Down to stereo at most (the first two channels), as the game plays it
				int outChannels = Math.Min(channels, 2);
				var result = new DecodedAudio { SampleRate = reader.SampleRate, Channels = new float[outChannels][] };
				for (int c = 0; c < outChannels; ++c)
					result.Channels[c] = new float[frames];
				int frame = 0, position = 0;
				foreach (float[] chunk in samples)
				{
					for (int i = 0; i < chunk.Length; ++i, ++position)
					{
						int c = position % channels;
						if (c < outChannels && frame < frames)
							result.Channels[c][frame] = chunk[i];
						if (c == channels - 1)
							++frame;
					}
				}
				return result;
			}
		}

		// Info without decoding: sample rate and length (for the memory estimate)
		public static void ProbeOgg(string path, out int sampleRate, out int channels, out long samples)
		{
			using (var reader = new NVorbis.VorbisReader(path))
			{
				sampleRate = reader.SampleRate;
				channels = reader.Channels;
				samples = reader.TotalSamples;
			}
		}

		private static short[] ToStereoInt16(float[][] channels)
		{
			int length = channels[0].Length;
			var result = new short[length * 2];
			for (int i = 0; i < length; ++i)
			{
				result[i * 2] = ToInt16(channels[0][i]);
				result[i * 2 + 1] = ToInt16(channels[channels.Length > 1 ? 1 : 0][i]);
			}
			return result;
		}

		private static short ToInt16(float value)
		{
			int v = (int)Math.Round(value * 32767.0f);
			return (short)Math.Max(-32768, Math.Min(32767, v));
		}

		public static void WriteWav(string path, short[] interleaved, int channels, int sampleRate)
		{
			using (var stream = new FileStream(path, FileMode.Create, FileAccess.Write))
			using (var writer = new BinaryWriter(stream))
			{
				int dataBytes = interleaved.Length * 2;
				writer.Write(Encoding.ASCII.GetBytes("RIFF"));
				writer.Write(36 + dataBytes);
				writer.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
				writer.Write(16);
				writer.Write((short)1);
				writer.Write((short)channels);
				writer.Write(sampleRate);
				writer.Write(sampleRate * channels * 2);
				writer.Write((short)(channels * 2));
				writer.Write((short)16);
				writer.Write(Encoding.ASCII.GetBytes("data"));
				writer.Write(dataBytes);
				var bytes = new byte[dataBytes];
				Buffer.BlockCopy(interleaved, 0, bytes, 0, dataBytes);
				writer.Write(bytes);
			}
		}

		// Music or jingle as ATRAC3 with padding: what follows "loopStart" (in 44.1 kHz samples) for music, silence for
		// anything else (loopStart < 0). Returns the 44.1 kHz samples before the padding.
		public static int ConvertToAtrac3(DecodedAudio audio, string output, int loopStart, string atracdenc, string tempFolder)
		{
			float[][] channels = Resampler.Resample(audio.Channels, audio.SampleRate, 44100);
			short[] pcm = ToStereoInt16(channels);
			int frames = pcm.Length / 2;
			var padded = new short[pcm.Length + Atrac3EndPadding * 2];
			Array.Copy(pcm, padded, pcm.Length);
			if (loopStart >= 0 && loopStart < frames)
			{
				int count = Math.Min(Atrac3EndPadding, frames - loopStart);
				Array.Copy(pcm, loopStart * 2, padded, pcm.Length, count * 2);
			}

			string wav = Path.Combine(tempFolder, Guid.NewGuid().ToString("N") + ".wav");
			string at3 = Path.ChangeExtension(wav, ".at3");		// (atracdenc picks the container by the extension: .at3 is RIFF)
			try
			{
				WriteWav(wav, padded, 2, 44100);
				RunAtracdenc(atracdenc, wav, at3);
				using (var file = File.OpenRead(at3))
				{
					var signature = new byte[4];
					if (file.Read(signature, 0, 4) != 4 || Encoding.ASCII.GetString(signature) != "RIFF")
						throw new InvalidDataException("atracdenc did not write an ATRAC3 RIFF file");
				}
				File.Copy(at3, output, true);
			}
			finally
			{
				TryDelete(wav);
				TryDelete(at3);
			}
			return frames;
		}

		// Sound effect as 22.05 kHz WAV, mono if both channels are (nearly) the same. Returns the sample bytes.
		public static long ConvertToWav(DecodedAudio audio, string output, out bool mono)
		{
			float[][] channels = Resampler.Resample(audio.Channels, audio.SampleRate, SoundRate);
			short[] stereo = ToStereoInt16(channels);
			mono = true;
			for (int i = 0; i < stereo.Length && mono; i += 2)
			{
				if (Math.Abs(stereo[i] - stereo[i + 1]) > 8)
					mono = false;
			}
			if (mono)
			{
				var samples = new short[stereo.Length / 2];
				for (int i = 0; i < samples.Length; ++i)
					samples[i] = (short)((stereo[i * 2] + stereo[i * 2 + 1]) >> 1);
				WriteWav(output, samples, 1, SoundRate);
				return samples.Length * 2L;
			}
			WriteWav(output, stereo, 2, SoundRate);
			return stereo.Length * 2L;
		}

		private static void RunAtracdenc(string atracdenc, string input, string output)
		{
			var info = new ProcessStartInfo(atracdenc, "-e atrac3 -i \"" + input + "\" -o \"" + output + "\"")
			{
				UseShellExecute = false,
				CreateNoWindow = true,
				RedirectStandardOutput = true,
				RedirectStandardError = true,
			};
			if (!File.Exists(atracdenc))
				throw new FileNotFoundException("atracdenc.exe is missing, it should be next to this program: " + atracdenc);
			using (var process = Process.Start(info))
			{
				// (read both outputs while it runs, it writes a progress bar)
				var errorTask = process.StandardError.ReadToEndAsync();
				string stdout = process.StandardOutput.ReadToEnd();
				process.WaitForExit();
				if (process.ExitCode != 0 || !File.Exists(output))
					throw new InvalidOperationException("atracdenc failed (exit code " + process.ExitCode + "): " + (errorTask.Result + stdout).Trim());
			}
		}

		// "audio_bank.bin": "S3AB", version 1, number of files, 0 (uint32 each), then per file: name (56 bytes,
		// zero-padded), offset, size; the files follow, each at a multiple of 2048 bytes (see PSPAudioBank in the engine)
		public static void WriteBank(string folder, IList<string> names)
		{
			var sorted = new List<string>(names);
			sorted.Sort(StringComparer.Ordinal);
			Func<long, long> align = value => (value + BankAlignment - 1) / BankAlignment * BankAlignment;
			long offset = align(16 + 64 * sorted.Count);
			var offsets = new List<long>();
			foreach (string name in sorted)
			{
				offsets.Add(offset);
				offset = align(offset + new FileInfo(Path.Combine(folder, name)).Length);
			}
			string path = Path.Combine(folder, "audio_bank.bin");
			using (var stream = new FileStream(path, FileMode.Create, FileAccess.Write))
			using (var writer = new BinaryWriter(stream))
			{
				writer.Write(Encoding.ASCII.GetBytes("S3AB"));
				writer.Write(1u);
				writer.Write((uint)sorted.Count);
				writer.Write(0u);
				for (int i = 0; i < sorted.Count; ++i)
				{
					byte[] encoded = Encoding.UTF8.GetBytes(sorted[i]);
					if (encoded.Length >= 56)
						throw new InvalidOperationException("File name too long for the audio bank: " + sorted[i]);
					var nameBytes = new byte[56];
					Array.Copy(encoded, nameBytes, encoded.Length);
					writer.Write(nameBytes);
					writer.Write((uint)offsets[i]);
					writer.Write((uint)new FileInfo(Path.Combine(folder, sorted[i])).Length);
				}
				for (int i = 0; i < sorted.Count; ++i)
				{
					writer.Flush();
					while (stream.Position < offsets[i])
						writer.Write((byte)0);
					writer.Write(File.ReadAllBytes(Path.Combine(folder, sorted[i])));
				}
			}
			foreach (string name in sorted)
				File.Delete(Path.Combine(folder, name));
		}

		public static void TryDelete(string path)
		{
			try
			{
				if (File.Exists(path))
					File.Delete(path);
			}
			catch (IOException) { }
			catch (UnauthorizedAccessException) { }
		}
	}
}
