using System.Diagnostics;
using System.Drawing;
using System.Text.Json;
using System.Threading;
using System.Windows.Forms;

namespace FH2VR.Standalone;

internal static class Program
{
    internal const string Version = "0.4.0-dev";

    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();

        using var mutex = new Mutex(true, @"Local\FH2VR.Standalone", out bool firstInstance);
        if (!firstInstance)
            return;

        using var log = FileLog.Open();
        log.Write($"FH2VR {Version} standalone launcher started");

        try
        {
            using var context = new TrayApplicationContext(log);
            Application.Run(context);
        }
        catch (Exception ex)
        {
            log.Write($"Fatal error: {ex}");
            MessageBox.Show(ex.Message, "FH2VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            log.Write("FH2VR standalone launcher stopped");
        }
    }
}

internal sealed class TrayApplicationContext : ApplicationContext
{
    private readonly FileLog _log;
    private readonly LauncherSettings _settings;
    private readonly NotifyIcon _tray;
    private readonly ToolStripMenuItem _status;
    private readonly ToolStripMenuItem _install;
    private readonly ToolStripMenuItem _restore;
    private readonly ToolStripMenuItem _launch;
    private readonly System.Windows.Forms.Timer _timer;
    private int _exiting;

    internal TrayApplicationContext(FileLog log)
    {
        _log = log;
        _settings = LauncherSettings.Load(log);

        Icon icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath ?? string.Empty)
                    ?? SystemIcons.Application;

        var menu = new ContextMenuStrip();
        menu.Items.Add(new ToolStripMenuItem($"FH2VR {Program.Version}") { Enabled = false });
        _status = new ToolStripMenuItem("Checking installation...") { Enabled = false };
        menu.Items.Add(_status);
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Select Azimuth folder...", null, (_, _) => SelectAzimuthFolder());
        _install = new ToolStripMenuItem("Install / Repair FH2VR", null, (_, _) => Install());
        _restore = new ToolStripMenuItem("Restore original Azimuth helper", null, (_, _) => Restore());
        _launch = new ToolStripMenuItem("Launch Azimuth", null, (_, _) => Launch());
        menu.Items.Add(_install);
        menu.Items.Add(_restore);
        menu.Items.Add(_launch);
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Open FH2VR logs", null, (_, _) => OpenLogs());
        menu.Items.Add("Exit", null, (_, _) => Exit());

        _tray = new NotifyIcon
        {
            Text = $"FH2VR {Program.Version}",
            Icon = icon,
            Visible = true,
            ContextMenuStrip = menu
        };

        _tray.DoubleClick += (_, _) => Launch();

        _timer = new System.Windows.Forms.Timer { Interval = 1000 };
        _timer.Tick += (_, _) => RefreshStatus();
        _timer.Start();

        if (string.IsNullOrWhiteSpace(_settings.AzimuthDirectory) ||
            !File.Exists(Path.Combine(_settings.AzimuthDirectory, "azimuth.exe")))
        {
            _status.Text = "Select your Azimuth folder";
        }
        else
        {
            RefreshStatus();
        }
    }

    private void SelectAzimuthFolder()
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Select the folder containing azimuth.exe",
            UseDescriptionForTitle = true,
            ShowNewFolderButton = false,
            InitialDirectory = _settings.AzimuthDirectory ?? string.Empty
        };

        if (dialog.ShowDialog() != DialogResult.OK)
            return;

        string selected = Path.GetFullPath(dialog.SelectedPath);
        if (!File.Exists(Path.Combine(selected, "azimuth.exe")))
        {
            MessageBox.Show("That folder does not contain azimuth.exe.",
                            "FH2VR", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        _settings.AzimuthDirectory = selected;
        _settings.Save(_log);
        _log.Write($"Azimuth directory selected: {selected}");
        RefreshStatus();
    }

    private void RefreshStatus()
    {
        string? root = ValidRoot();
        if (root is null)
        {
            _status.Text = "Azimuth folder not configured";
            _install.Enabled = false;
            _restore.Enabled = false;
            _launch.Enabled = false;
            SetTrayText("FH2VR — select Azimuth folder");
            return;
        }

        _install.Enabled = !AzimuthRunning();
        _launch.Enabled = true;

        string bin = Path.Combine(root, "data", "bin");
        string runtime = Path.Combine(bin, "FH2VR.dll");
        string backup = Path.Combine(bin, "azimuth-tls.dll.fh2vr-original");
        string state = Path.Combine(root, "FH2VR", "installation.json");
        bool installed = File.Exists(runtime) && File.Exists(backup) && File.Exists(state);
        bool running = AzimuthRunning();

        _restore.Enabled = File.Exists(backup) && !running;

        if (running && installed)
            _status.Text = "Azimuth running · FH2VR installed";
        else if (running)
            _status.Text = "Azimuth running · stop it before installing";
        else if (installed)
            _status.Text = "FH2VR installed · ready";
        else
            _status.Text = "FH2VR standalone not installed";

        SetTrayText("FH2VR — " + _status.Text);
    }

    private void Install()
    {
        try
        {
            string root = RequireRoot();
            if (AzimuthRunning())
                throw new InvalidOperationException("Close Azimuth before installing or repairing FH2VR.");

            string payload = Path.Combine(AppContext.BaseDirectory, "Payload");
            string payloadRuntime = Path.Combine(payload, "FH2VR.dll");
            string payloadBootstrap = Path.Combine(payload, "FH2VR.Bootstrap.dll");

            if (!File.Exists(payloadRuntime))
                throw new FileNotFoundException("FH2VR runtime payload is missing.", payloadRuntime);
            if (!File.Exists(payloadBootstrap))
                throw new FileNotFoundException(
                    "The standalone bootstrap payload is not included in this build yet. " +
                    "The launcher and FH2VR standalone runtime are ready, but the Azimuth startup bridge still needs to be added.",
                    payloadBootstrap);

            string bin = Path.Combine(root, "data", "bin");
            Directory.CreateDirectory(bin);

            string liveHelper = Path.Combine(bin, "azimuth-tls.dll");
            string backupHelper = Path.Combine(bin, "azimuth-tls.dll.fh2vr-original");
            string runtimeTarget = Path.Combine(bin, "FH2VR.dll");
            string stateDirectory = Path.Combine(root, "FH2VR");
            string statePath = Path.Combine(stateDirectory, "installation.json");

            if (!File.Exists(liveHelper))
                throw new InvalidOperationException("Azimuth data/bin/azimuth-tls.dll was not found.");

            if (!File.Exists(backupHelper))
            {
                File.Copy(liveHelper, backupHelper, overwrite: false);
                _log.Write("Backed up original azimuth-tls.dll");
            }

            string stagedRuntime = runtimeTarget + ".new";
            string stagedBootstrap = liveHelper + ".new";
            File.Copy(payloadRuntime, stagedRuntime, overwrite: true);
            File.Copy(payloadBootstrap, stagedBootstrap, overwrite: true);

            File.Move(stagedRuntime, runtimeTarget, overwrite: true);
            File.Move(stagedBootstrap, liveHelper, overwrite: true);

            Directory.CreateDirectory(stateDirectory);
            var state = new InstallationState
            {
                Version = Program.Version,
                InstalledUtc = DateTimeOffset.UtcNow,
                AzimuthDirectory = root,
                RuntimePath = Path.GetRelativePath(root, runtimeTarget),
                OriginalHelperBackup = Path.GetRelativePath(root, backupHelper)
            };
            File.WriteAllText(statePath, JsonSerializer.Serialize(state, JsonOptions.Pretty));

            _log.Write("FH2VR standalone installed successfully");
            MessageBox.Show("FH2VR standalone installed. Launch Azimuth, then press F1 in game.",
                            "FH2VR", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception ex)
        {
            _log.Write($"Install failed: {ex.Message}");
            MessageBox.Show(ex.Message, "FH2VR install failed",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            RefreshStatus();
        }
    }

    private void Restore()
    {
        try
        {
            string root = RequireRoot();
            if (AzimuthRunning())
                throw new InvalidOperationException("Close Azimuth before restoring its original helper.");

            string bin = Path.Combine(root, "data", "bin");
            string liveHelper = Path.Combine(bin, "azimuth-tls.dll");
            string backupHelper = Path.Combine(bin, "azimuth-tls.dll.fh2vr-original");
            string runtimeTarget = Path.Combine(bin, "FH2VR.dll");
            string stateDirectory = Path.Combine(root, "FH2VR");

            if (!File.Exists(backupHelper))
                throw new InvalidOperationException("No FH2VR original-helper backup exists for this Azimuth installation.");

            File.Copy(backupHelper, liveHelper, overwrite: true);
            File.Delete(backupHelper);
            if (File.Exists(runtimeTarget))
                File.Delete(runtimeTarget);
            if (Directory.Exists(stateDirectory))
                Directory.Delete(stateDirectory, recursive: true);

            _log.Write("Restored original Azimuth helper and removed standalone FH2VR runtime");
            MessageBox.Show("Original Azimuth helper restored.",
                            "FH2VR", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception ex)
        {
            _log.Write($"Restore failed: {ex.Message}");
            MessageBox.Show(ex.Message, "FH2VR restore failed",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            RefreshStatus();
        }
    }

    private void Launch()
    {
        try
        {
            string root = RequireRoot();
            string exe = Path.Combine(root, "azimuth.exe");
            Process.Start(new ProcessStartInfo
            {
                FileName = exe,
                WorkingDirectory = root,
                UseShellExecute = true
            });
            _log.Write("Launched Azimuth");
        }
        catch (Exception ex)
        {
            _log.Write($"Launch failed: {ex.Message}");
            MessageBox.Show(ex.Message, "FH2VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void OpenLogs()
    {
        try
        {
            Directory.CreateDirectory(FileLog.LogDirectory);
            Process.Start(new ProcessStartInfo
            {
                FileName = FileLog.LogDirectory,
                UseShellExecute = true
            });
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "FH2VR", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private string? ValidRoot()
    {
        string? root = _settings.AzimuthDirectory;
        return !string.IsNullOrWhiteSpace(root) && File.Exists(Path.Combine(root, "azimuth.exe"))
            ? root
            : null;
    }

    private string RequireRoot()
        => ValidRoot() ?? throw new InvalidOperationException("Select the Azimuth folder first.");

    private static bool AzimuthRunning()
    {
        try
        {
            Process[] processes = Process.GetProcessesByName("azimuth");
            foreach (Process process in processes)
                process.Dispose();
            return processes.Length != 0;
        }
        catch
        {
            return false;
        }
    }

    private void SetTrayText(string text)
    {
        _tray.Text = text.Length <= 63 ? text : text[..63];
    }

    private void Exit()
    {
        if (Interlocked.Exchange(ref _exiting, 1) != 0)
            return;

        _timer.Stop();
        _tray.Visible = false;
        ExitThread();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _timer.Dispose();
            _tray.Dispose();
        }
        base.Dispose(disposing);
    }
}

internal sealed class LauncherSettings
{
    public string? AzimuthDirectory { get; set; }

    internal static LauncherSettings Load(FileLog log)
    {
        try
        {
            if (File.Exists(SettingsPath))
            {
                return JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(SettingsPath), JsonOptions.Pretty)
                       ?? new LauncherSettings();
            }
        }
        catch (Exception ex)
        {
            log.Write($"Could not load settings: {ex.Message}");
        }

        return new LauncherSettings();
    }

    internal void Save(FileLog log)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
            File.WriteAllText(SettingsPath, JsonSerializer.Serialize(this, JsonOptions.Pretty));
        }
        catch (Exception ex)
        {
            log.Write($"Could not save settings: {ex.Message}");
        }
    }

    private static string SettingsPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                     "FH2VR", "settings.json");
}

internal sealed class InstallationState
{
    public string Version { get; set; } = string.Empty;
    public DateTimeOffset InstalledUtc { get; set; }
    public string AzimuthDirectory { get; set; } = string.Empty;
    public string RuntimePath { get; set; } = string.Empty;
    public string OriginalHelperBackup { get; set; } = string.Empty;
}

internal static class JsonOptions
{
    internal static readonly JsonSerializerOptions Pretty = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true
    };
}

internal sealed class FileLog : IDisposable
{
    private const long MaxBytes = 2L * 1024L * 1024L;
    private readonly object _gate = new();
    private readonly StreamWriter _writer;

    private FileLog(string path, StreamWriter writer)
    {
        FilePath = path;
        _writer = writer;
    }

    internal static string LogDirectory =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                     "FH2VR", "logs");

    internal string FilePath { get; }

    internal static FileLog Open()
    {
        Directory.CreateDirectory(LogDirectory);
        string path = Path.Combine(LogDirectory, "FH2VR-Launcher.log");

        if (File.Exists(path) && new FileInfo(path).Length > MaxBytes)
        {
            string rotated = path + ".1";
            if (File.Exists(rotated))
                File.Delete(rotated);
            File.Move(path, rotated);
        }

        var stream = new FileStream(path, FileMode.Append, FileAccess.Write,
                                    FileShare.ReadWrite | FileShare.Delete);
        var writer = new StreamWriter(stream) { AutoFlush = true };
        return new FileLog(path, writer);
    }

    internal void Write(string message)
    {
        lock (_gate)
            _writer.WriteLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff}] {message}");
    }

    public void Dispose()
    {
        lock (_gate)
            _writer.Dispose();
    }
}
