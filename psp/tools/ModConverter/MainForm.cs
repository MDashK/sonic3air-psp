using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace S3AirPspModConverter
{
	sealed class MainForm : Form
	{
		private readonly TextBox mSourceBox = new TextBox { Anchor = AnchorStyles.Left | AnchorStyles.Right };
		private readonly Button mSourceBrowse = new Button { Text = "Browse...", AutoSize = true };
		private readonly Button mScanButton = new Button { Text = "Scan", AutoSize = true };
		private readonly ListView mList = new ListView { View = View.Details, CheckBoxes = true, FullRowSelect = true, Dock = DockStyle.Fill, HideSelection = false };
		private readonly TextBox mDetails = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill };
		private readonly TextBox mOutputBox = new TextBox { Anchor = AnchorStyles.Left | AnchorStyles.Right };
		private readonly Button mOutputBrowse = new Button { Text = "Browse...", AutoSize = true };
		private readonly Label mMemoryLabel = new Label { AutoSize = true, Dock = DockStyle.Fill, Padding = new Padding(0, 4, 0, 4) };
		private readonly ProgressBar mProgress = new ProgressBar { Dock = DockStyle.Fill, Maximum = 1000 };
		private readonly Button mConvertButton = new Button { Text = "Convert checked mods", AutoSize = true, Enabled = false };
		private readonly Button mCancelButton = new Button { Text = "Cancel", AutoSize = true, Enabled = false };
		private readonly TextBox mLog = new TextBox { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font(FontFamily.GenericMonospace, 8.5f) };

		private readonly List<ModInfo> mMods = new List<ModInfo>();
		private CancellationTokenSource mCancel;
		private bool mBusy;

		private static string SettingsPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "S3AIR-PSP-ModConverter", "settings.txt");

		public MainForm()
		{
			Text = "Sonic 3 A.I.R. PSP Mod Converter";
			try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (Exception) { }
			Font = new Font("Segoe UI", 9f);
			ClientSize = new Size(980, 760);
			MinimumSize = new Size(760, 600);
			StartPosition = FormStartPosition.CenterScreen;

			mList.Columns.Add("Mod", 250);
			mList.Columns.Add("Author", 120);
			mList.Columns.Add("Contents", 230);
			mList.Columns.Add("PSP memory", 90, HorizontalAlignment.Right);
			mList.Columns.Add("Memory stick", 90, HorizontalAlignment.Right);
			mList.Columns.Add("Status", 250);

			var layout = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(10) };
			layout.Controls.Add(Heading("1. Your mods on the PC: the game's mods folder, any folder with mods (also .zip files), or a single mod"));
			layout.Controls.Add(Row(mSourceBox, mSourceBrowse, mScanButton));
			layout.Controls.Add(Heading("2. The mods to convert (select one for details)"));
			layout.Controls.Add(mList);
			layout.Controls.Add(mDetails);
			layout.Controls.Add(mMemoryLabel);
			layout.Controls.Add(Heading("3. Where the converted mods go: on the memory stick, PSP/GAME/Sonic3AIR/savedata/mods"));
			layout.Controls.Add(Row(mOutputBox, mOutputBrowse));
			var bottom = new TableLayoutPanel { ColumnCount = 3, Dock = DockStyle.Fill, AutoSize = true };
			bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
			bottom.Controls.Add(mProgress, 0, 0);
			bottom.Controls.Add(mConvertButton, 1, 0);
			bottom.Controls.Add(mCancelButton, 2, 0);
			layout.Controls.Add(bottom);
			layout.Controls.Add(mLog);
			for (int i = 0; i < layout.Controls.Count; ++i)
				layout.RowStyles.Add(new RowStyle(SizeType.AutoSize));
			layout.RowStyles[3] = new RowStyle(SizeType.Percent, 50);
			layout.RowStyles[4] = new RowStyle(SizeType.Absolute, 90);
			layout.RowStyles[layout.Controls.Count - 1] = new RowStyle(SizeType.Percent, 50);
			Controls.Add(layout);

			mSourceBrowse.Click += (s, e) => BrowseFolder(mSourceBox, "The folder with your mods, or a single mod's folder");
			mOutputBrowse.Click += (s, e) => BrowseFolder(mOutputBox, "PSP/GAME/Sonic3AIR/savedata/mods on the memory stick (or any folder to copy over later)");
			mScanButton.Click += (s, e) => Scan();
			mConvertButton.Click += (s, e) => ConvertChecked();
			mCancelButton.Click += (s, e) => mCancel?.Cancel();
			mList.SelectedIndexChanged += (s, e) => ShowDetails();
			mList.ItemCheck += OnItemCheck;
			mList.ItemChecked += (s, e) => UpdateMemoryLabel();
			mSourceBox.KeyDown += (s, e) => { if (e.KeyCode == Keys.Enter) Scan(); };

			LoadSettings();
			UpdateMemoryLabel();
			Log("Converts Sonic 3 A.I.R. mods for the PSP port: sprites get packed for loading without memory peaks, music and sounds get converted to the PSP's formats, and each mod gets its memory need written down (psp.json), which the game checks.");
			Log("The PSP has " + Mods.FormatKB(Mods.MemoryBudget) + " of memory for mods: all active mods together must fit in there. Mods that use more can't be made active.");
			if (!File.Exists(Program.AtracdencPath))
				Log("atracdenc.exe is missing next to this program: music can't be converted.");
			FormClosing += (s, e) => { SaveSettings(); mCancel?.Cancel(); };
		}

		private static Label Heading(string text) => new Label { Text = text, AutoSize = true, Font = new Font("Segoe UI", 9f, FontStyle.Bold), Padding = new Padding(0, 8, 0, 2) };

		private static Control Row(TextBox box, params Button[] buttons)
		{
			var row = new TableLayoutPanel { ColumnCount = 1 + buttons.Length, Dock = DockStyle.Fill, AutoSize = true };
			row.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
			row.Controls.Add(box, 0, 0);
			for (int i = 0; i < buttons.Length; ++i)
			{
				row.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
				row.Controls.Add(buttons[i], i + 1, 0);
			}
			return row;
		}

		private void BrowseFolder(TextBox box, string description)
		{
			using (var dialog = new FolderBrowserDialog { Description = description, ShowNewFolderButton = true })
			{
				if (Directory.Exists(box.Text))
					dialog.SelectedPath = box.Text;
				if (dialog.ShowDialog(this) == DialogResult.OK)
				{
					box.Text = dialog.SelectedPath;
					if (box == mSourceBox)
						Scan();
				}
			}
		}

		private void Log(string line)
		{
			if (InvokeRequired)
			{
				BeginInvoke(new Action<string>(Log), line);
				return;
			}
			mLog.AppendText(line + Environment.NewLine);
		}

		private void SetBusy(bool busy)
		{
			mBusy = busy;
			mScanButton.Enabled = !busy;
			mSourceBrowse.Enabled = !busy;
			mOutputBrowse.Enabled = !busy;
			mConvertButton.Enabled = !busy && mMods.Count > 0;
			mCancelButton.Enabled = busy;
		}

		private async void Scan()
		{
			string source = mSourceBox.Text.Trim();
			if (!Directory.Exists(source))
			{
				MessageBox.Show(this, "Choose the folder with your mods first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
				return;
			}
			SetBusy(true);
			mList.Items.Clear();
			mMods.Clear();
			mDetails.Clear();
			mProgress.Value = 0;
			Log("Scanning " + source + " ...");
			try
			{
				List<ModInfo> found = await Task.Run(() => Mods.FindMods(source, Log));
				if (found.Count == 0)
					Log("No mods found there (a mod is a folder with a mod.json file in it).");
				for (int i = 0; i < found.Count; ++i)
				{
					ModInfo mod = found[i];
					await Task.Run(() => Mods.Analyze(mod));
					mMods.Add(mod);
					AddListItem(mod);
					mProgress.Value = (int)(1000.0 * (i + 1) / found.Count);
				}
				Log("Found " + found.Count + " mods.");
			}
			catch (Exception e)
			{
				Log("Scanning failed: " + e.Message);
			}
			SetBusy(false);
			UpdateMemoryLabel();
		}

		private void AddListItem(ModInfo mod)
		{
			var item = new ListViewItem(mod.DisplayName + (mod.ModVersion.Length > 0 ? "  (" + mod.ModVersion + ")" : "")) { Tag = mod };
			item.SubItems.Add(mod.Author);
			item.SubItems.Add(mod.Error != null ? "-" : mod.ContentSummary);
			item.SubItems.Add(mod.Error != null ? "-" : Mods.FormatKB(mod.BudgetBytes));
			item.SubItems.Add(mod.Error != null ? "-" : Mods.FormatMB(mod.OutputBytesEstimate));
			string status;
			if (mod.Error != null)
			{
				status = "Can't be converted: " + mod.Error;
				item.ForeColor = Color.Firebrick;
			}
			else if (!mod.FitsBudget)
			{
				status = "Too big for the PSP's memory";
				item.ForeColor = Color.Firebrick;
			}
			else if (mod.Warnings.Count > 0)
			{
				status = "OK, with " + mod.Warnings.Count + (mod.Warnings.Count == 1 ? " note" : " notes") + ": " + mod.Warnings[0];
				item.ForeColor = Color.DarkOrange;
			}
			else
			{
				status = "OK";
			}
			item.SubItems.Add(status);
			item.Checked = mod.Error == null && mod.FitsBudget;
			mList.Items.Add(item);
		}

		private void OnItemCheck(object sender, ItemCheckEventArgs e)
		{
			if (e.NewValue != CheckState.Checked)
				return;
			if (mBusy)
			{
				e.NewValue = e.CurrentValue;
				return;
			}
			var mod = (ModInfo)mList.Items[e.Index].Tag;
			if (mod.Error != null || !mod.FitsBudget)
				e.NewValue = CheckState.Unchecked;
		}

		private void UpdateMemoryLabel()
		{
			var checkedMods = mList.CheckedItems.Cast<ListViewItem>().Select(i => (ModInfo)i.Tag).ToList();
			// (the same rule as the game: what the mods need all the time, plus the sprite cache they need at least)
			long total = checkedMods.Sum(m => m.MemoryBytes) + Math.Min(checkedMods.Sum(m => m.SpritePixelBytes), Mods.SpriteCacheMin);
			mMemoryLabel.Text = checkedMods.Count == 0
				? "Memory on the PSP: " + Mods.FormatKB(Mods.MemoryBudget) + " for all active mods together."
				: "Checked: " + checkedMods.Count + " mods, " + Mods.FormatKB(total) + ". On the PSP, the mods you make active in the Mods menu must fit into " + Mods.FormatKB(Mods.MemoryBudget) + " together" +
				  (total > Mods.MemoryBudget ? " (these don't all fit at once, the game lets you pick)." : ".");
			mMemoryLabel.ForeColor = total > Mods.MemoryBudget ? Color.DarkOrange : SystemColors.ControlText;
			mConvertButton.Enabled = !mBusy && checkedMods.Count > 0;
		}

		private void ShowDetails()
		{
			if (mList.SelectedItems.Count == 0)
			{
				mDetails.Clear();
				return;
			}
			var mod = (ModInfo)mList.SelectedItems[0].Tag;
			var lines = new List<string>();
			lines.Add(mod.DisplayName + (mod.Author.Length > 0 ? " by " + mod.Author : "") + "   [" + mod.SourceName + "]");
			if (mod.Error != null)
				lines.Add("Can't be converted: " + mod.Error);
			else
			{
				lines.Add("Memory on the PSP: " + Mods.FormatKB(mod.BudgetBytes) + " of the " + Mods.FormatKB(Mods.MemoryBudget) + " for mods. All the time: " + Mods.FormatKB(mod.MemoryBytes) +
						  " (sprite objects " + Mods.FormatKB(mod.Sprites.ResidentBytes) + ", sounds " + Mods.FormatKB(mod.SoundBytesEstimate) + ", scripts " + Mods.FormatKB(mod.ScriptSourceBytes * 2) +
						  ", other data " + Mods.FormatKB(mod.OtherDataBytes) + ")" + (mod.SpritePixelBytes > 0 ? ", plus the sprite cache" : "") + ". Music and jingles are streamed, they need almost no memory.");
				lines.AddRange(mod.Warnings.Select(w => "! " + w));
				lines.AddRange(mod.Notes.Select(n => "- " + n));
			}
			mDetails.Text = string.Join(Environment.NewLine, lines);
		}

		private async void ConvertChecked()
		{
			string output = mOutputBox.Text.Trim();
			if (output.Length == 0)
			{
				MessageBox.Show(this, "Choose where the converted mods go first (step 3).", Text, MessageBoxButtons.OK, MessageBoxIcon.Information);
				return;
			}
			try
			{
				Directory.CreateDirectory(output);
			}
			catch (Exception e)
			{
				MessageBox.Show(this, "Can't use that output folder: " + e.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
				return;
			}
			var mods = mList.CheckedItems.Cast<ListViewItem>().Select(i => (ModInfo)i.Tag).ToList();
			if (mods.Any(m => m.NumMusic + m.NumJingles > 0) && !File.Exists(Program.AtracdencPath))
			{
				MessageBox.Show(this, "atracdenc.exe is missing next to this program, it's needed for the music.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
				return;
			}

			SetBusy(true);
			mCancel = new CancellationTokenSource();
			CancellationToken cancel = mCancel.Token;
			int converted = 0, failed = 0;
			for (int i = 0; i < mods.Count && !cancel.IsCancellationRequested; ++i)
			{
				ModInfo mod = mods[i];
				int index = i;
				Log("Converting " + mod.DisplayName + " ...");
				try
				{
					await Task.Run(() => Mods.Convert(mod, output, Program.AtracdencPath, Log,
						p => BeginInvoke(new Action(() => mProgress.Value = Math.Min(1000, (int)(1000.0 * (index + p) / mods.Count)))), cancel));
					Log("  Done: " + Path.Combine(output, mod.DirectoryName) + " (memory on the PSP: " + Mods.FormatKB(mod.BudgetBytes) + ")");
					++converted;
				}
				catch (OperationCanceledException)
				{
					Log("  Cancelled.");
				}
				catch (Exception e)
				{
					Log("  Failed: " + e.Message);
					++failed;
				}
			}
			mProgress.Value = 1000;
			Log("Converted " + converted + " of " + mods.Count + " mods" + (failed > 0 ? ", " + failed + " failed" : "") + ".");
			if (converted > 0)
				Log("On the PSP: start the game, open \"Mods\" in the main menu and make the mods active there.");
			SetBusy(false);
			UpdateMemoryLabel();
			SaveSettings();
		}

		private void LoadSettings()
		{
			try
			{
				if (File.Exists(SettingsPath))
				{
					string[] lines = File.ReadAllLines(SettingsPath);
					if (lines.Length > 0) mSourceBox.Text = lines[0];
					if (lines.Length > 1) mOutputBox.Text = lines[1];
				}
			}
			catch (Exception) { }

			if (mOutputBox.Text.Length == 0)
			{
				// A memory stick with the game on it?
				foreach (DriveInfo drive in DriveInfo.GetDrives())
				{
					try
					{
						if (drive.IsReady && File.Exists(Path.Combine(drive.RootDirectory.FullName, "PSP", "GAME", "Sonic3AIR", "EBOOT.PBP")))
						{
							mOutputBox.Text = Path.Combine(drive.RootDirectory.FullName, "PSP", "GAME", "Sonic3AIR", "savedata", "mods");
							break;
						}
					}
					catch (Exception) { }
				}
			}
		}

		private void SaveSettings()
		{
			try
			{
				Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath));
				File.WriteAllLines(SettingsPath, new[] { mSourceBox.Text, mOutputBox.Text });
			}
			catch (Exception) { }
		}
	}
}
