using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace S3AirPspModConverter
{
	// A small JSON reader that accepts what the game accepts: // and /* */ comments, trailing commas.
	// Objects keep their members like the game's JSON library (jsoncpp) does: a later duplicate replaces the earlier one,
	// and iterating goes by key in byte order (see SortedMembers), which decides e.g. which sprite of a sheet comes first.
	abstract class JsonNode
	{
		// The value as the game's "asString()" gives it (numbers and booleans as text), null for objects and arrays
		public virtual string AsString() => null;
	}

	sealed class JsonString : JsonNode
	{
		public readonly string Value;
		public JsonString(string value) { Value = value; }
		public override string AsString() => Value;
	}

	sealed class JsonNumber : JsonNode
	{
		public readonly string Text;
		public JsonNumber(string text) { Text = text; }
		public override string AsString() => Text;
	}

	sealed class JsonBool : JsonNode
	{
		public readonly bool Value;
		public JsonBool(bool value) { Value = value; }
		public override string AsString() => Value ? "true" : "false";
	}

	sealed class JsonNull : JsonNode
	{
		public override string AsString() => "";
	}

	sealed class JsonArray : JsonNode
	{
		public readonly List<JsonNode> Items = new List<JsonNode>();
	}

	sealed class JsonObject : JsonNode
	{
		// In the file's order (for writing it again), duplicates replaced in place
		public readonly List<KeyValuePair<string, JsonNode>> Members = new List<KeyValuePair<string, JsonNode>>();

		public void Set(string key, JsonNode value)
		{
			for (int i = 0; i < Members.Count; ++i)
			{
				if (Members[i].Key == key)
				{
					Members[i] = new KeyValuePair<string, JsonNode>(key, value);
					return;
				}
			}
			Members.Add(new KeyValuePair<string, JsonNode>(key, value));
		}

		public JsonNode Get(string key)
		{
			foreach (var member in Members)
			{
				if (member.Key == key)
					return member.Value;
			}
			return null;
		}

		public string GetString(string key) => Get(key)?.AsString();

		// The order the game goes through the members: by key, comparing the UTF-8 bytes
		public List<KeyValuePair<string, JsonNode>> SortedMembers()
		{
			var sorted = new List<KeyValuePair<string, JsonNode>>(Members);
			sorted.Sort((a, b) => CompareUtf8(a.Key, b.Key));
			return sorted;
		}

		public static int CompareUtf8(string a, string b)
		{
			byte[] x = Encoding.UTF8.GetBytes(a);
			byte[] y = Encoding.UTF8.GetBytes(b);
			int length = Math.Min(x.Length, y.Length);
			for (int i = 0; i < length; ++i)
			{
				if (x[i] != y[i])
					return x[i] < y[i] ? -1 : 1;
			}
			return x.Length.CompareTo(y.Length);
		}
	}

	static class Json
	{
		public static JsonNode Parse(string text)
		{
			var parser = new Parser(text);
			parser.SkipWhitespace();
			JsonNode result = parser.ParseValue();
			parser.SkipWhitespace();
			if (!parser.AtEnd)
				throw parser.Error("unexpected text after the end");
			return result;
		}

		public static JsonNode Load(string path)
		{
			string text = System.IO.File.ReadAllText(path, Encoding.UTF8);
			if (text.Length > 0 && text[0] == '﻿')
				text = text.Substring(1);
			try
			{
				return Parse(text);
			}
			catch (FormatException e)
			{
				throw new FormatException(System.IO.Path.GetFileName(path) + ": " + e.Message);
			}
		}

		public static string Quote(string value)
		{
			var sb = new StringBuilder("\"");
			foreach (char c in value)
			{
				switch (c)
				{
					case '"': sb.Append("\\\""); break;
					case '\\': sb.Append("\\\\"); break;
					case '\n': sb.Append("\\n"); break;
					case '\r': sb.Append("\\r"); break;
					case '\t': sb.Append("\\t"); break;
					default:
						if (c < 0x20)
							sb.Append("\\u").Append(((int)c).ToString("x4"));
						else
							sb.Append(c);
						break;
				}
			}
			return sb.Append('"').ToString();
		}

		private sealed class Parser
		{
			private readonly string mText;
			private int mPos;

			public Parser(string text) { mText = text; }

			public bool AtEnd => mPos >= mText.Length;

			public FormatException Error(string message)
			{
				int line = 1;
				for (int i = 0; i < mPos && i < mText.Length; ++i)
				{
					if (mText[i] == '\n')
						++line;
				}
				return new FormatException("JSON error in line " + line + ": " + message);
			}

			public void SkipWhitespace()
			{
				while (mPos < mText.Length)
				{
					char c = mText[mPos];
					if (char.IsWhiteSpace(c))
					{
						++mPos;
					}
					else if (c == '/' && mPos + 1 < mText.Length && mText[mPos + 1] == '/')
					{
						while (mPos < mText.Length && mText[mPos] != '\n')
							++mPos;
					}
					else if (c == '/' && mPos + 1 < mText.Length && mText[mPos + 1] == '*')
					{
						int end = mText.IndexOf("*/", mPos + 2, StringComparison.Ordinal);
						mPos = (end < 0) ? mText.Length : end + 2;
					}
					else
					{
						break;
					}
				}
			}

			public JsonNode ParseValue()
			{
				if (AtEnd)
					throw Error("unexpected end");
				char c = mText[mPos];
				if (c == '{')
					return ParseObject();
				if (c == '[')
					return ParseArray();
				if (c == '"')
					return new JsonString(ParseString());
				if (c == '-' || (c >= '0' && c <= '9'))
				{
					int start = mPos;
					++mPos;
					while (mPos < mText.Length && "0123456789+-.eE".IndexOf(mText[mPos]) >= 0)
						++mPos;
					return new JsonNumber(mText.Substring(start, mPos - start));
				}
				if (Match("true"))
					return new JsonBool(true);
				if (Match("false"))
					return new JsonBool(false);
				if (Match("null"))
					return new JsonNull();
				throw Error("unexpected character '" + c + "'");
			}

			private bool Match(string word)
			{
				if (string.CompareOrdinal(mText, mPos, word, 0, word.Length) == 0)
				{
					mPos += word.Length;
					return true;
				}
				return false;
			}

			private JsonObject ParseObject()
			{
				var obj = new JsonObject();
				++mPos;
				while (true)
				{
					SkipWhitespace();
					if (AtEnd)
						throw Error("unexpected end in an object");
					if (mText[mPos] == '}')
					{
						++mPos;
						return obj;
					}
					if (mText[mPos] != '"')
						throw Error("expected a key in quotes");
					string key = ParseString();
					SkipWhitespace();
					if (AtEnd || mText[mPos] != ':')
						throw Error("expected ':' after \"" + key + "\"");
					++mPos;
					SkipWhitespace();
					obj.Set(key, ParseValue());
					SkipWhitespace();
					if (!AtEnd && mText[mPos] == ',')
						++mPos;
					else if (!AtEnd && mText[mPos] != '}')
						throw Error("expected ',' or '}' after \"" + key + "\"");
				}
			}

			private JsonArray ParseArray()
			{
				var array = new JsonArray();
				++mPos;
				while (true)
				{
					SkipWhitespace();
					if (AtEnd)
						throw Error("unexpected end in an array");
					if (mText[mPos] == ']')
					{
						++mPos;
						return array;
					}
					array.Items.Add(ParseValue());
					SkipWhitespace();
					if (!AtEnd && mText[mPos] == ',')
						++mPos;
					else if (!AtEnd && mText[mPos] != ']')
						throw Error("expected ',' or ']'");
				}
			}

			private string ParseString()
			{
				var sb = new StringBuilder();
				++mPos;
				while (true)
				{
					if (AtEnd)
						throw Error("unexpected end in a string");
					char c = mText[mPos++];
					if (c == '"')
						return sb.ToString();
					if (c != '\\')
					{
						sb.Append(c);
						continue;
					}
					if (AtEnd)
						throw Error("unexpected end in a string");
					char e = mText[mPos++];
					switch (e)
					{
						case 'n': sb.Append('\n'); break;
						case 'r': sb.Append('\r'); break;
						case 't': sb.Append('\t'); break;
						case 'b': sb.Append('\b'); break;
						case 'f': sb.Append('\f'); break;
						case 'u':
							if (mPos + 4 > mText.Length)
								throw Error("bad \\u escape");
							sb.Append((char)int.Parse(mText.Substring(mPos, 4), NumberStyles.HexNumber));
							mPos += 4;
							break;
						default: sb.Append(e); break;
					}
				}
			}
		}
	}
}
