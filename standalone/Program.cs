using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;
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
        log.Write($"FH2VR {Version} standalone injector started");

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
            log.Write("FH2VR standalone injector stopped");
        }
    }
}

internal sealed class TrayApplicationContext : ApplicationContext
{
    private readonly FileLog _log;
    private readonly LauncherSettings _settings;
    private readonly NotifyIcon _tray;
    private readonly ToolStripMenuItem _status;
    private readonly ToolStripMenuItem _attach;
    private readonly ToolStripMenuItem _launch;
    private readonly System.Windows.Forms.Timer _timer;

    private int? _attachedPid;
    private int? _lastAttemptPid;
    private DateTime _nextAttemptUtc = DateTime.MinValue;
    private int _exiting;

    internal TrayApplicationContext(FileLog log)
    {
        _log = log;
        _settings = LauncherSettings.Load(log);

        Icon icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath ?? string.Empty)
                    ?? SystemIcons.Application;

        var menu = new ContextMenuStrip();
        menu.Items.Add(new ToolStripMenuItem($"FH2VR {Program.Version}") { Enabled = false });
        _status = new ToolStripMenuItem("Waiting for Azimuth...") { Enabled = false };
        menu.Items.Add(_status);
        menu.Items.Add(new ToolStripSeparator());
        _attach = new ToolStripMenuItem("Attach now", null, (_, _) => AttachNow(showErrors: true));
        _launch = new ToolStripMenuItem("Launch Azimuth", null, (_, _) => LaunchAzimuth());
        menu.Items.Add(_attach);
        menu.Items.Add(_launch);
        menu.Items.Add("Select Azimuth folder...", null, (_, _) => SelectAzimuthFolder());
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
        _tray.DoubleClick += (_, _) => AttachNow(showErrors: true);

        _timer = new System.Windows.Forms.Timer { Interval = 1000 };
        _timer.Tick += (_, _) => Tick();
        _timer.Start();

        Tick();
    }

    private string RuntimePath
        => Path.Combine(AppContext.BaseDirectory, "Payload", "FH2VR.dll");

    private void Tick()
    {
        Process? process = FindNewestAzimuth();
        if (process is null)
        {
            _attachedPid = null;
            _lastAttemptPid = null;
            _status.Text = "Waiting for azimuth.exe";
            _attach.Enabled = false;
            SetTrayText("FH2VR — waiting for Azimuth");
            return;
        }

        using (process)
        {
            _attach.Enabled = true;

            if (_attachedPid == process.Id)
            {
                _status.Text = $"Attached · PID {process.Id} · F1 VR / F2 GUI";
                SetTrayText("FH2VR — attached");
                return;
            }

            if (RemoteRuntime.IsModuleLoaded(process.Id, "FH2VR.dll"))
            {
                _attachedPid = process.Id;
                _status.Text = $"FH2VR already loaded · PID {process.Id}";
                SetTrayText("FH2VR — runtime loaded");
                return;
            }

            _status.Text = $"Azimuth detected · PID {process.Id} · attaching...";
            SetTrayText("FH2VR — attaching");

            if ((_lastAttemptPid != process.Id || DateTime.UtcNow >= _nextAttemptUtc) &&
                File.Exists(RuntimePath))
            {
                _lastAttemptPid = process.Id;
                _nextAttemptUtc = DateTime.UtcNow.AddSeconds(5);
                TryAttach(process, showErrors: false);
            }
            else if (!File.Exists(RuntimePath))
            {
                _status.Text = "Payload/FH2VR.dll is missing";
                SetTrayText("FH2VR — runtime missing");
            }
        }
    }

    private void AttachNow(bool showErrors)
    {
        Process? process = FindNewestAzimuth();
        if (process is null)
        {
            if (showErrors)
                MessageBox.Show("azimuth.exe is not running.", "FH2VR",
                                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        using (process)
            TryAttach(process, showErrors);
    }

    private void TryAttach(Process process, bool showErrors)
    {
        try
        {
            if (!File.Exists(RuntimePath))
                throw new FileNotFoundException("Standalone runtime is missing from Payload/FH2VR.dll.", RuntimePath);

            if (RemoteRuntime.IsModuleLoaded(process.Id, "FH2VR.dll"))
            {
                _attachedPid = process.Id;
                _log.Write($"PID {process.Id}: FH2VR.dll is already loaded; not injecting a duplicate");
                return;
            }

            _log.Write($"Attaching FH2VR runtime to azimuth PID {process.Id}");
            RemoteRuntime.LoadAndStart(process.Id, RuntimePath, "FH2VR_StandaloneInitialize");
            _attachedPid = process.Id;
            _lastAttemptPid = process.Id;
            _status.Text = $"Attached · PID {process.Id} · F1 VR / F2 GUI";
            SetTrayText("FH2VR — attached");
            _log.Write($"Standalone runtime initialized in PID {process.Id}");

            _tray.BalloonTipTitle = "FH2VR attached";
            _tray.BalloonTipText = "Press F1 for VR and F2 for the manual GUI/HUD overlay.";
            _tray.ShowBalloonTip(2500);
        }
        catch (Exception ex)
        {
            _attachedPid = null;
            _status.Text = "Attach failed · see logs";
            SetTrayText("FH2VR — attach failed");
            _log.Write($"Attach to PID {process.Id} failed: {ex}");

            if (showErrors)
            {
                MessageBox.Show(
                    ex.Message + "\n\nIf Azimuth is running as administrator, run FH2VR with the same privilege level.",
                    "FH2VR attach failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
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
    }

    private void LaunchAzimuth()
    {
        try
        {
            string? root = _settings.AzimuthDirectory;
            if (string.IsNullOrWhiteSpace(root) || !File.Exists(Path.Combine(root, "azimuth.exe")))
            {
                SelectAzimuthFolder();
                root = _settings.AzimuthDirectory;
            }

            if (string.IsNullOrWhiteSpace(root))
                return;

            Process.Start(new ProcessStartInfo
            {
                FileName = Path.Combine(root, "azimuth.exe"),
                WorkingDirectory = root,
                UseShellExecute = true
            });
            _log.Write("Launched Azimuth; auto-attach will run when the process is ready");
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

    private static Process? FindNewestAzimuth()
    {
        Process[] all = Process.GetProcessesByName("azimuth");
        Process? selected = null;
        DateTime newest = DateTime.MinValue;

        foreach (Process process in all)
        {
            try
            {
                DateTime start = process.StartTime;
                if (start > newest)
                {
                    selected?.Dispose();
                    selected = process;
                    newest = start;
                }
                else
                {
                    process.Dispose();
                }
            }
            catch
            {
                process.Dispose();
            }
        }

        return selected;
    }

    private void SetTrayText(string text)
        => _tray.Text = text.Length <= 63 ? text : text[..63];

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

internal static class RemoteRuntime
{
    private const uint ProcessCreateThread = 0x0002;
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessVmOperation = 0x0008;
    private const uint ProcessVmWrite = 0x0020;
    private const uint ProcessVmRead = 0x0010;
    private const uint MemCommit = 0x1000;
    private const uint MemReserve = 0x2000;
    private const uint MemRelease = 0x8000;
    private const uint PageReadWrite = 0x04;
    private const uint WaitObject0 = 0x00000000;
    private const uint WaitTimeout = 0x00000102;
    private const uint Infinite = 0xFFFFFFFF;
    private const uint Th32csSnapModule = 0x00000008;
    private const uint Th32csSnapModule32 = 0x00000010;
    private const uint DontResolveDllReferences = 0x00000001;

    internal static void LoadAndStart(int processId, string runtimePath, string exportName)
    {
        string fullPath = Path.GetFullPath(runtimePath);
        IntPtr process = OpenProcess(
            ProcessCreateThread | ProcessQueryInformation | ProcessVmOperation | ProcessVmWrite | ProcessVmRead,
            false, processId);
        if (process == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed");

        try
        {
            byte[] pathBytes = Encoding.Unicode.GetBytes(fullPath + "\0");
            IntPtr remotePath = VirtualAllocEx(process, IntPtr.Zero, (nuint)pathBytes.Length,
                                               MemCommit | MemReserve, PageReadWrite);
            if (remotePath == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "VirtualAllocEx failed");

            try
            {
                if (!WriteProcessMemory(process, remotePath, pathBytes, (nuint)pathBytes.Length, out nuint written) ||
                    written != (nuint)pathBytes.Length)
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "WriteProcessMemory failed");

                IntPtr remoteLoadLibrary = RemoteProcAddress(processId, "kernel32.dll", "LoadLibraryW");
                RunRemoteThread(process, remoteLoadLibrary, remotePath, "LoadLibraryW");
            }
            finally
            {
                VirtualFreeEx(process, remotePath, 0, MemRelease);
            }

            IntPtr remoteRuntime = WaitForModule(processId, Path.GetFileName(fullPath), TimeSpan.FromSeconds(5));
            if (remoteRuntime == IntPtr.Zero)
                throw new InvalidOperationException("FH2VR.dll was not visible in the Azimuth module list after loading.");

            IntPtr localRuntime = LoadLibraryExW(fullPath, IntPtr.Zero, DontResolveDllReferences);
            if (localRuntime == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not map FH2VR.dll locally to resolve its standalone export");

            try
            {
                IntPtr localExport = GetProcAddress(localRuntime, exportName);
                if (localExport == IntPtr.Zero)
                    throw new InvalidOperationException($"FH2VR runtime does not export {exportName}.");

                long exportRva = localExport.ToInt64() - localRuntime.ToInt64();
                IntPtr remoteExport = new(remoteRuntime.ToInt64() + exportRva);
                uint result = RunRemoteThread(process, remoteExport, IntPtr.Zero, exportName);
                if (result == 0)
                    throw new InvalidOperationException($"{exportName} returned failure inside Azimuth.");
            }
            finally
            {
                FreeLibrary(localRuntime);
            }
        }
        finally
        {
            CloseHandle(process);
        }
    }

    internal static bool IsModuleLoaded(int processId, string moduleName)
        => FindModule(processId, moduleName) != IntPtr.Zero;

    private static IntPtr WaitForModule(int processId, string moduleName, TimeSpan timeout)
    {
        Stopwatch stopwatch = Stopwatch.StartNew();
        while (stopwatch.Elapsed < timeout)
        {
            IntPtr module = FindModule(processId, moduleName);
            if (module != IntPtr.Zero)
                return module;
            Thread.Sleep(25);
        }
        return IntPtr.Zero;
    }

    private static IntPtr RemoteProcAddress(int processId, string moduleName, string procName)
    {
        IntPtr remoteModule = FindModule(processId, moduleName);
        if (remoteModule == IntPtr.Zero)
            throw new InvalidOperationException($"Remote module {moduleName} was not found.");

        IntPtr localModule = GetModuleHandleW(moduleName);
        if (localModule == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"Local module {moduleName} was not found");

        IntPtr localProc = GetProcAddress(localModule, procName);
        if (localProc == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"Local export {procName} was not found");

        long rva = localProc.ToInt64() - localModule.ToInt64();
        return new IntPtr(remoteModule.ToInt64() + rva);
    }

    private static IntPtr FindModule(int processId, string moduleName)
    {
        IntPtr snapshot = CreateToolhelp32Snapshot(Th32csSnapModule | Th32csSnapModule32, (uint)processId);
        if (snapshot == new IntPtr(-1))
            return IntPtr.Zero;

        try
        {
            MODULEENTRY32 entry = new() { dwSize = (uint)Marshal.SizeOf<MODULEENTRY32>() };
            if (!Module32FirstW(snapshot, ref entry))
                return IntPtr.Zero;

            do
            {
                if (string.Equals(entry.szModule, moduleName, StringComparison.OrdinalIgnoreCase))
                    return entry.modBaseAddr;
                entry.dwSize = (uint)Marshal.SizeOf<MODULEENTRY32>();
            }
            while (Module32NextW(snapshot, ref entry));

            return IntPtr.Zero;
        }
        finally
        {
            CloseHandle(snapshot);
        }
    }

    private static uint RunRemoteThread(IntPtr process, IntPtr startAddress, IntPtr parameter, string operation)
    {
        IntPtr thread = CreateRemoteThread(process, IntPtr.Zero, 0, startAddress, parameter, 0, out _);
        if (thread == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error(), $"CreateRemoteThread failed for {operation}");

        try
        {
            uint wait = WaitForSingleObject(thread, 10000);
            if (wait == WaitTimeout)
                throw new TimeoutException($"Remote {operation} thread timed out.");
            if (wait != WaitObject0)
                throw new Win32Exception(Marshal.GetLastWin32Error(), $"Waiting for remote {operation} thread failed");
            if (!GetExitCodeThread(thread, out uint exitCode))
                throw new Win32Exception(Marshal.GetLastWin32Error(), $"Could not read remote {operation} result");
            return exitCode;
        }
        finally
        {
            CloseHandle(thread);
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MODULEENTRY32
    {
        public uint dwSize;
        public uint th32ModuleID;
        public uint th32ProcessID;
        public uint GlblcntUsage;
        public uint ProccntUsage;
        public IntPtr modBaseAddr;
        public uint modBaseSize;
        public IntPtr hModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szModule;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string szExePath;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(IntPtr process, IntPtr address, nuint size, uint allocationType, uint protect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, nuint size, uint freeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(IntPtr process, IntPtr address, byte[] buffer, nuint size, out nuint written);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(IntPtr process, IntPtr attributes, nuint stackSize,
                                                     IntPtr startAddress, IntPtr parameter,
                                                     uint creationFlags, out uint threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeThread(IntPtr thread, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Module32FirstW(IntPtr snapshot, ref MODULEENTRY32 entry);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Module32NextW(IntPtr snapshot, ref MODULEENTRY32 entry);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandleW(string moduleName);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procName);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeLibrary(IntPtr module);
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
