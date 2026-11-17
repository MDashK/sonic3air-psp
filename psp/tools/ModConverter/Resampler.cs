using System;
using System.Threading.Tasks;

namespace S3AirPspModConverter
{
	// Sample rate conversion with a Kaiser-windowed sinc filter (the same quality class as the soxr resampler that
	// convert_audio.py uses through ffmpeg): flat up to ~20 kHz, no audible aliasing.
	static class Resampler
	{
		private const int ZeroCrossings = 32;		// Filter half length, in zero crossings of the sinc
		private const int TableResolution = 512;	// Kernel values per zero crossing (linear interpolation in between)
		private const double KaiserBeta = 9.0;

		// Per channel; returns the same arrays if nothing needs to change
		public static float[][] Resample(float[][] channels, int inRate, int outRate)
		{
			if (inRate == outRate || channels.Length == 0)
				return channels;

			double ratio = (double)outRate / inRate;
			double scale = Math.Min(1.0, ratio);			// Lower the cutoff when downsampling
			double cutoff = scale * 0.96;					// Relative to the input's Nyquist frequency
			int halfLength = (int)Math.Ceiling(ZeroCrossings / scale);	// In input samples

			// Kernel as a function of the distance in input samples, sampled TableResolution times per input sample
			int tableSize = halfLength * TableResolution + 2;
			var table = new float[tableSize];
			double i0Beta = BesselI0(KaiserBeta);
			for (int i = 0; i < tableSize; ++i)
			{
				double x = (double)i / TableResolution;
				double window = (x >= halfLength) ? 0.0 : BesselI0(KaiserBeta * Math.Sqrt(1.0 - (x / halfLength) * (x / halfLength))) / i0Beta;
				double arg = Math.PI * cutoff * x;
				double sinc = (x == 0.0) ? 1.0 : Math.Sin(arg) / arg;
				table[i] = (float)(cutoff * sinc * window);
			}

			int inLength = channels[0].Length;
			int outLength = (int)Math.Ceiling(inLength * ratio);
			var result = new float[channels.Length][];
			Parallel.For(0, channels.Length, c =>
			{
				float[] input = channels[c];
				var output = new float[outLength];
				for (int n = 0; n < outLength; ++n)
				{
					double position = n / ratio;
					int center = (int)Math.Floor(position);
					double fraction = position - center;
					int first = Math.Max(center - halfLength + 1, 0);
					int last = Math.Min(center + halfLength, inLength - 1);
					float sum = 0.0f;
					for (int k = first; k <= last; ++k)
					{
						double distance = Math.Abs(k - position) * TableResolution;
						int index = (int)distance;
						if (index + 1 >= tableSize)
							continue;
						float t = (float)(distance - index);
						sum += input[k] * (table[index] + (table[index + 1] - table[index]) * t);
					}
					output[n] = sum;
				}
				result[c] = output;
			});
			return result;
		}

		private static double BesselI0(double x)
		{
			double sum = 1.0, term = 1.0, halfX = x / 2.0;
			for (int k = 1; k < 50; ++k)
			{
				term *= (halfX / k) * (halfX / k);
				sum += term;
				if (term < sum * 1e-12)
					break;
			}
			return sum;
		}
	}
}
