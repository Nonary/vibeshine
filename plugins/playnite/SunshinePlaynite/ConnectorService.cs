using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Playnite.SDK;
using Playnite.SDK.Models;
using Playnite.SDK.Plugins;

namespace SunshinePlaynite
{
    internal sealed class ConnectorService : IDisposable
    {
        private const string ControlPipeName = "Sunshine.PlayniteExtension";
        private const int DataConnectionTimeoutMs = 5000;
        private const int HelloTimeoutMs = 5000;
        private const int ShutdownFlushTimeoutMs = 500;
        private const uint ProcessQueryLimitedInformation = 0x1000;
        private readonly IPlayniteAPI api;
        private readonly ConnectorLog log;
        private readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        private readonly object lifecycleLock = new object();
        private readonly object coreLock = new object();
        private readonly object gameStateLock = new object();
        private readonly object snapshotLock = new object();
        private readonly ConcurrentDictionary<string, PipeConnection> launchers = new ConcurrentDictionary<string, PipeConnection>();
        private readonly ConcurrentDictionary<Guid, byte> sunshineGames = new ConcurrentDictionary<Guid, byte>();
        private readonly ConcurrentDictionary<Guid, byte> pendingGames = new ConcurrentDictionary<Guid, byte>();
        private readonly EnvironmentScopes environmentScopes = new EnvironmentScopes();
        private CancellationTokenSource cancellation;
        private Task serverTask;
        private PipeConnection core;
        private NamedPipeServerStream pendingPipe;
        private Timer snapshotTimer;
        private int started;
        private int libraryNotificationsEnabled = 1;

        public ConnectorService(IPlayniteAPI api, ILogger logger, bool enableDebugLogging)
        {
            this.api = api;
            log = new ConnectorLog(logger, enableDebugLogging);
        }

        public void ApplySettings(bool notifyLibraryChanges, bool enableDebugLogging)
        {
            Volatile.Write(ref libraryNotificationsEnabled, notifyLibraryChanges ? 1 : 0);
            log.SetDebugEnabled(enableDebugLogging);
        }

        public void Start()
        {
            lock (lifecycleLock)
            {
                if (Volatile.Read(ref started) != 0) return;
                var nextCancellation = new CancellationTokenSource();
                try
                {
                    cancellation = nextCancellation;
                    api.Database.Games.ItemCollectionChanged += GamesChanged;
                    api.Database.Games.ItemUpdated += GamesUpdated;
                    snapshotTimer = new Timer(_ => SendSnapshot(), null, Timeout.Infinite, Timeout.Infinite);
                    serverTask = Task.Factory.StartNew(() => ServerLoop(nextCancellation.Token), nextCancellation.Token,
                        TaskCreationOptions.LongRunning, TaskScheduler.Default);
                    Volatile.Write(ref started, 1);
                    log.Info("Compiled Playnite plugin started");
                }
                catch
                {
                    try { api.Database.Games.ItemCollectionChanged -= GamesChanged; } catch { }
                    try { api.Database.Games.ItemUpdated -= GamesUpdated; } catch { }
                    try { if (snapshotTimer != null) snapshotTimer.Dispose(); } catch { }
                    snapshotTimer = null;
                    serverTask = null;
                    cancellation = null;
                    nextCancellation.Dispose();
                    throw;
                }
            }
        }

        public void Stop()
        {
            lock (lifecycleLock)
            {
                if (Volatile.Read(ref started) == 0) return;
                Volatile.Write(ref started, 0);
                log.Info("Beginning shutdown");
                try
                {
                    try { SendShutdownHandoff(); }
                    catch (Exception ex) { log.Warn("Shutdown handoff failed: " + ex.Message); }
                    try { FlushConnections(ShutdownFlushTimeoutMs); }
                    catch (Exception ex) { log.Warn("Shutdown flush failed: " + ex.Message); }
                }
                finally
                {
                    try { api.Database.Games.ItemCollectionChanged -= GamesChanged; } catch { }
                    try { api.Database.Games.ItemUpdated -= GamesUpdated; } catch { }

                    var timer = snapshotTimer;
                    snapshotTimer = null;
                    try { if (timer != null) timer.Dispose(); } catch { }

                    var stopCancellation = cancellation;
                    try { if (stopCancellation != null) stopCancellation.Cancel(); } catch { }
                    try { if (pendingPipe != null) pendingPipe.Dispose(); } catch { }
                    pendingPipe = null;
                    try { ReplaceCore(null); } catch { }
                    foreach (var item in launchers.ToArray())
                    {
                        PipeConnection ignored;
                        if (launchers.TryRemove(item.Key, out ignored))
                        {
                            try { ignored.Dispose(); } catch { }
                        }
                        try { environmentScopes.Clear(item.Key); } catch { }
                    }
                    lock (gameStateLock)
                    {
                        pendingGames.Clear();
                        sunshineGames.Clear();
                    }

                    var task = serverTask;
                    serverTask = null;
                    try { if (task != null) task.Wait(1000); } catch { }
                    cancellation = null;
                    try { if (stopCancellation != null) stopCancellation.Dispose(); } catch { }
                    log.Info("Connector stopped");
                }
            }
        }

        public void Dispose()
        {
            Stop();
            log.Dispose();
        }

        public void GameStarted(Game game)
        {
            if (game == null) return;
            log.Info("Game started: " + game.Name + " [" + game.Id + "]");
            var payload = RunOnUi(() => BuildStatus("gameStarted", game), true);
            lock (gameStateLock)
            {
                if (launchers.Values.Any(x => SameId(x.GameId, game.Id))) sunshineGames.TryAdd(game.Id, 0);
                SendCore(payload);
                Broadcast(payload, null);
            }
        }

        public void GameStopped(Game game)
        {
            if (game == null) return;
            log.Info("Game stopped: " + game.Name + " [" + game.Id + "]");
            var payload = RunOnUi(() => BuildStatus("gameStopped", game), true);
            lock (gameStateLock)
            {
                byte ignored;
                pendingGames.TryRemove(game.Id, out ignored);
                if (!sunshineGames.TryRemove(game.Id, out ignored))
                {
                    log.Debug("Ignoring stop status for an untracked game: " + game.Id);
                    return;
                }
                SendCore(payload);
                Broadcast(payload, null);
            }
        }

        public void QueueSnapshot()
        {
            var timer = snapshotTimer;
            if (timer != null) timer.Change(3000, Timeout.Infinite);
        }

        private void GamesChanged(object sender, ItemCollectionChangedEventArgs<Game> args)
        {
            if (Volatile.Read(ref libraryNotificationsEnabled) != 0) QueueSnapshot();
        }

        private void GamesUpdated(object sender, ItemUpdatedEventArgs<Game> args)
        {
            if (Volatile.Read(ref libraryNotificationsEnabled) != 0) QueueSnapshot();
        }

        private void ServerLoop(CancellationToken token)
        {
            log.Info("Pipe server starting");
            var security = CreatePipeSecurity();
            while (!token.IsCancellationRequested)
            {
                NamedPipeServerStream control = null;
                NamedPipeServerStream data = null;
                try
                {
                    control = CreateServer(ControlPipeName, security);
                    pendingPipe = control;
                    WaitForConnection(control, token);
                    var pipeName = Guid.NewGuid().ToString("B").ToUpperInvariant();
                    data = CreateServer(pipeName, security);
                    WriteHandshake(control, pipeName);
                    if (!WaitForAck(control)) throw new IOException("Handshake ACK missing");
                    control.Dispose();
                    control = null;
                    pendingPipe = data;
                    WaitForConnection(data, token, DataConnectionTimeoutMs);

                    var reader = new StreamReader(data, new UTF8Encoding(false), false, 8192, true);
                    var writer = new StreamWriter(data, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
                    var helloLine = ReadLine(reader, token, HelloTimeoutMs);
                    if (helloLine == null) throw new IOException("No hello received");
                    var hello = ParseObject(helloLine);
                    var role = ValidateClient(data, hello);
                    var connection = new PipeConnection(pipeName, data, reader, writer, log);
                    data = null;
                    if (string.Equals(role, "sunshine", StringComparison.OrdinalIgnoreCase))
                    {
                        ReplaceCore(connection);
                        StartCore(connection, token);
                    }
                    else
                    {
                        connection.Pid = GetInt(hello, "pid");
                        connection.GameId = GetString(hello, "gameId");
                        launchers[pipeName] = connection;
                        StartLauncher(connection, token);
                    }
                }
                catch (OperationCanceledException) { break; }
                catch (TimeoutException ex)
                {
                    if (!token.IsCancellationRequested) log.Warn("Pipe handshake timed out: " + ex.Message);
                }
                catch (Exception ex)
                {
                    if (!token.IsCancellationRequested) log.Warn("Pipe connection failed: " + ex.Message);
                }
                finally
                {
                    pendingPipe = null;
                    if (control != null) control.Dispose();
                    if (data != null) data.Dispose();
                }
            }
            log.Info("Pipe server exiting");
        }

        private static NamedPipeServerStream CreateServer(string name, PipeSecurity security)
        {
            const PipeOptions options = PipeOptions.Asynchronous;
            if (security == null) throw new ArgumentNullException(nameof(security));
            return new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                options, 65536, 65536, security);
        }

        private static PipeSecurity CreatePipeSecurity()
        {
            SecurityIdentifier user;
            using (var identity = WindowsIdentity.GetCurrent())
            {
                user = identity.User;
            }
            if (user == null) throw new InvalidOperationException("Could not resolve the Playnite user SID");

            var security = new PipeSecurity();
            security.SetAccessRuleProtection(true, false);
            security.SetOwner(user);
            security.AddAccessRule(new PipeAccessRule(
                new SecurityIdentifier(WellKnownSidType.NetworkSid, null),
                PipeAccessRights.ReadWrite,
                AccessControlType.Deny));
            security.AddAccessRule(new PipeAccessRule(
                new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null),
                PipeAccessRights.FullControl,
                AccessControlType.Allow));
            security.AddAccessRule(new PipeAccessRule(user, PipeAccessRights.FullControl, AccessControlType.Allow));
            return security;
        }

        private static void WaitForConnection(NamedPipeServerStream pipe, CancellationToken token, int timeoutMs = Timeout.Infinite)
        {
            var wait = pipe.WaitForConnectionAsync();
            var elapsed = Stopwatch.StartNew();
            while (!wait.Wait(200))
            {
                token.ThrowIfCancellationRequested();
                if (timeoutMs != Timeout.Infinite && elapsed.ElapsedMilliseconds >= timeoutMs)
                    throw new TimeoutException("Client did not connect to the data pipe");
            }
            if (!pipe.IsConnected) throw new IOException("Pipe failed to connect");
        }

        private static string ReadLine(StreamReader reader, CancellationToken token, int timeoutMs)
        {
            var read = reader.ReadLineAsync();
            var elapsed = Stopwatch.StartNew();
            while (!read.Wait(200))
            {
                token.ThrowIfCancellationRequested();
                if (elapsed.ElapsedMilliseconds >= timeoutMs)
                    throw new TimeoutException("Client did not send a hello message");
            }
            return read.GetAwaiter().GetResult();
        }

        private static string ValidateClient(NamedPipeServerStream pipe, IDictionary<string, object> hello)
        {
            if (!string.Equals(GetString(hello, "type"), "hello", StringComparison.Ordinal))
                throw new UnauthorizedAccessException("Invalid client hello message");

            var role = GetString(hello, "role");
            if (!string.Equals(role, "sunshine", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(role, "launcher", StringComparison.OrdinalIgnoreCase))
                throw new UnauthorizedAccessException("Invalid client role");

            uint actualPid;
            var claimedPid = GetInt(hello, "pid");
            if (!claimedPid.HasValue || claimedPid.Value <= 0 ||
                !GetNamedPipeClientProcessId(pipe.SafePipeHandle, out actualPid) ||
                actualPid != (uint)claimedPid.Value)
                throw new UnauthorizedAccessException("Client PID validation failed");

            var executable = GetProcessExecutableName(actualPid);
            var validExecutable = string.Equals(role, "launcher", StringComparison.OrdinalIgnoreCase)
                ? string.Equals(executable, "playnite-launcher.exe", StringComparison.OrdinalIgnoreCase)
                : string.Equals(executable, "sunshine.exe", StringComparison.OrdinalIgnoreCase) ||
                  string.Equals(executable, "vibeshine.exe", StringComparison.OrdinalIgnoreCase);
            if (!validExecutable)
                throw new UnauthorizedAccessException("Client executable does not match its declared role");

            return role;
        }

        private static string GetProcessExecutableName(uint processId)
        {
            var process = OpenProcess(ProcessQueryLimitedInformation, false, processId);
            if (process == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not inspect the pipe client process");
            try
            {
                var capacity = 32768;
                var path = new StringBuilder(capacity);
                if (!QueryFullProcessImageName(process, 0, path, ref capacity))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not resolve the pipe client executable");
                return Path.GetFileName(path.ToString());
            }
            finally
            {
                CloseHandle(process);
            }
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetNamedPipeClientProcessId(
            Microsoft.Win32.SafeHandles.SafePipeHandle pipe,
            out uint clientProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, uint processId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool QueryFullProcessImageName(
            IntPtr process,
            int flags,
            StringBuilder executablePath,
            ref int size);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);

        private static void WriteHandshake(Stream control, string pipeName)
        {
            var chars = new char[40];
            var source = (pipeName.ToUpperInvariant() + '\0').ToCharArray();
            Array.Copy(source, chars, Math.Min(source.Length, chars.Length));
            var bytes = Encoding.Unicode.GetBytes(chars);
            control.Write(bytes, 0, bytes.Length);
            control.Flush();
        }

        private static bool WaitForAck(Stream control)
        {
            var buffer = new byte[1];
            var read = control.ReadAsync(buffer, 0, 1);
            return read.Wait(1500) && read.Result == 1 && buffer[0] == 0x02;
        }

        private void StartCore(PipeConnection connection, CancellationToken token)
        {
            log.Info("Sunshine core connection accepted");
            Task.Factory.StartNew(() =>
            {
                try
                {
                    SendSnapshot();
                    SendRunningGames();
                    ReadCore(connection, token);
                }
                finally
                {
                    lock (coreLock)
                    {
                        if (ReferenceEquals(core, connection)) core = null;
                    }
                    connection.Dispose();
                }
            }, token, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        private void ReadCore(PipeConnection connection, CancellationToken token)
        {
            while (!token.IsCancellationRequested && ReferenceEquals(GetCore(), connection))
            {
                var line = connection.Reader.ReadLine();
                if (line == null) break;
                try { HandleCoreCommand(ParseObject(line)); }
                catch (Exception ex) { log.Warn("Failed to handle Sunshine command: " + ex.Message); }
            }
        }

        private void HandleCoreCommand(Dictionary<string, object> message)
        {
            if (GetString(message, "type") != "command") return;
            var command = GetString(message, "command");
            var id = GetString(message, "id");
            if (command == "launch" && Guid.TryParse(id, out var gameId))
            {
                lock (gameStateLock) sunshineGames.TryAdd(gameId, 0);
                var environment = GetEnvironment(message);
                RunOnUi(() => environmentScopes.RunTemporary(environment, () => api.StartGame(gameId)), false);
            }
            else if (command == "stop")
            {
                SendStopRequested(id);
            }
            else if (command == "snapshot")
            {
                SendSnapshot();
            }
            else if (command == "set-cover")
            {
                var success = false;
                var error = string.Empty;
                try
                {
                    var path = GetString(message, "path");
                    if (!Guid.TryParse(id, out gameId) || !File.Exists(path)) throw new IOException("Invalid game ID or cover path");
                    success = RunOnUi(() => SetCover(gameId, path), true);
                    if (!success) throw new InvalidOperationException("Playnite rejected the cover metadata update");
                }
                catch (Exception ex) { error = ex.Message; }
                SendCore(new Dictionary<string, object>
                {
                    ["type"] = "commandResult", ["command"] = "set-cover",
                    ["requestId"] = GetString(message, "requestId"), ["success"] = success, ["error"] = error
                });
            }
        }

        private void StartLauncher(PipeConnection connection, CancellationToken token)
        {
            log.Info(string.Format("Launcher connection accepted id={0} pid={1}", connection.Id, connection.Pid));
            SyncLauncher(connection);
            FlushPending(connection);
            Task.Factory.StartNew(() =>
            {
                try
                {
                    while (!token.IsCancellationRequested)
                    {
                        var line = connection.Reader.ReadLine();
                        if (line == null) break;
                        var message = ParseObject(line);
                        if (GetString(message, "type") != "command") continue;
                        var command = GetString(message, "command");
                        if (command == "launch" && Guid.TryParse(GetString(message, "id"), out var gameId))
                        {
                            lock (gameStateLock) sunshineGames.TryAdd(gameId, 0);
                            environmentScopes.Set(connection.Id, GetEnvironment(message));
                            RunOnUi(() => api.StartGame(gameId), false);
                        }
                        else if (command == "set-environment")
                        {
                            environmentScopes.Set(connection.Id, GetEnvironment(message));
                        }
                    }
                }
                catch (Exception ex) { if (!token.IsCancellationRequested) log.Warn("Launcher reader failed: " + ex.Message); }
                finally
                {
                    PipeConnection ignored;
                    launchers.TryRemove(connection.Id, out ignored);
                    environmentScopes.Clear(connection.Id);
                    connection.Dispose();
                    log.Info("Launcher disconnected: " + connection.Id);
                }
            }, token, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        private void SendSnapshot()
        {
            lock (snapshotLock)
            {
                var target = GetCore();
                if (target == null) return;
                try
                {
                    var snapshot = RunOnUi(BuildSnapshot, true);
                    var messages = new List<string>
                    {
                        Serialize(new Dictionary<string, object> { ["type"] = "snapshotStart" }),
                        Serialize(new Dictionary<string, object> { ["type"] = "plugins", ["payload"] = snapshot.Plugins }),
                        Serialize(new Dictionary<string, object> { ["type"] = "categories", ["payload"] = snapshot.Categories })
                    };
                    if (snapshot.Games.Count == 0)
                    {
                        messages.Add(Serialize(new Dictionary<string, object>
                        {
                            ["type"] = "games", ["payload"] = new object[0]
                        }));
                    }
                    else
                    {
                        for (var index = 0; index < snapshot.Games.Count; index += 100)
                        {
                            messages.Add(Serialize(new Dictionary<string, object>
                            {
                                ["type"] = "games", ["payload"] = snapshot.Games.Skip(index).Take(100).ToArray()
                            }));
                        }
                    }
                    messages.Add(Serialize(new Dictionary<string, object>
                    {
                        ["type"] = "snapshotComplete", ["games"] = snapshot.Games.Count
                    }));
                    if (!target.SendBatch(messages))
                    {
                        log.Warn("Snapshot connection closed before it could be queued");
                        return;
                    }
                    log.Info(string.Format("Snapshot completed: categories={0} games={1}", snapshot.Categories.Count, snapshot.Games.Count));
                }
                catch (Exception ex) { log.Warn("Snapshot failed: " + ex.Message); }
            }
        }

        private Snapshot BuildSnapshot()
        {
            var categories = api.Database.Categories.Select(x => new Dictionary<string, object>
            {
                ["id"] = x.Id.ToString(), ["name"] = x.Name
            }).ToList();
            var categoryNames = api.Database.Categories.ToDictionary(x => x.Id, x => x.Name);
            var pluginNames = new Dictionary<Guid, string>();
            foreach (var plugin in api.Addons.Plugins.OfType<LibraryPlugin>()) pluginNames[plugin.Id] = plugin.Name;
            var plugins = pluginNames.OrderBy(x => x.Value).Select(x => new Dictionary<string, object>
            {
                ["id"] = x.Key.ToString(), ["name"] = x.Value
            }).ToList();
            var games = api.Database.Games.Select(game => BuildGame(game, categoryNames, pluginNames)).ToList();
            return new Snapshot(categories, plugins, games);
        }

        private Dictionary<string, object> BuildGame(Game game, IDictionary<Guid, string> categories, IDictionary<Guid, string> plugins)
        {
            var action = GetAction(game);
            var categoryList = (game.CategoryIds ?? new List<Guid>()).Where(categories.ContainsKey).Select(x => categories[x]).ToArray();
            string pluginName;
            plugins.TryGetValue(game.PluginId, out pluginName);
            return new Dictionary<string, object>
            {
                ["id"] = game.Id.ToString(), ["name"] = game.Name,
                ["exe"] = action.Path, ["args"] = action.Arguments, ["workingDir"] = action.WorkingDirectory,
                ["installDir"] = game.InstallDirectory ?? string.Empty, ["categories"] = categoryList,
                ["pluginId"] = game.PluginId == Guid.Empty ? string.Empty : game.PluginId.ToString(),
                ["pluginName"] = pluginName ?? string.Empty, ["playtimeMinutes"] = (int)(game.Playtime / 60),
                ["lastPlayed"] = game.LastActivity.HasValue ? game.LastActivity.Value.ToString("o") : string.Empty,
                ["boxArtPath"] = FullPath(game.CoverImage), ["iconPath"] = FullPath(game.Icon),
                ["installed"] = game.IsInstalled, ["tags"] = new string[0]
            };
        }

        private ActionInfo GetAction(Game game)
        {
            var action = game.GameActions == null ? null : game.GameActions.FirstOrDefault(x => x.IsPlayAction) ?? game.GameActions.FirstOrDefault();
            return new ActionInfo
            {
                Path = action == null ? string.Empty : action.Path ?? string.Empty,
                Arguments = action == null ? string.Empty : action.Arguments ?? string.Empty,
                WorkingDirectory = action != null && !string.IsNullOrEmpty(action.WorkingDir) ? action.WorkingDir : game.InstallDirectory ?? string.Empty,
                Source = action
            };
        }

        private string FullPath(string path)
        {
            if (string.IsNullOrEmpty(path)) return string.Empty;
            try { return api.Database.GetFullFilePath(path); } catch { return string.Empty; }
        }

        private bool SetCover(Guid gameId, string sourcePath)
        {
            var game = api.Database.Games.Get(gameId);
            if (game == null) return false;
            var old = game.CoverImage;
            var imported = api.Database.AddFile(sourcePath, gameId);
            try
            {
                game.CoverImage = imported;
                api.Database.Games.Update(game);
                return true;
            }
            catch
            {
                game.CoverImage = old;
                try { api.Database.RemoveFile(imported); } catch { }
                throw;
            }
        }

        private string BuildStatus(string name, Game game)
        {
            var action = GetAction(game);
            var installDirectory = game.InstallDirectory ?? string.Empty;
            if (action.Source != null && action.Source.Type.ToString().IndexOf("Emulator", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                var emulator = api.Database.Emulators.Get(action.Source.EmulatorId);
                if (emulator != null && !string.IsNullOrEmpty(emulator.InstallDir)) installDirectory = emulator.InstallDir;
            }
            return Serialize(new Dictionary<string, object>
            {
                ["type"] = "status",
                ["status"] = new Dictionary<string, object>
                {
                    ["name"] = name, ["id"] = game.Id.ToString(), ["installDir"] = installDirectory, ["exe"] = action.Path
                }
            });
        }

        private void SendStopRequested(string id)
        {
            var payload = Serialize(new Dictionary<string, object>
            {
                ["type"] = "status", ["status"] = new Dictionary<string, object> { ["name"] = "stopRequested", ["id"] = id ?? string.Empty }
            });
            var matches = launchers.Values.Where(x => SameId(x.GameId, id)).ToArray();
            Broadcast(payload, matches.Length == 0 ? null : matches);
        }

        private void SendRunningGames()
        {
            foreach (var game in RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true))
            {
                var payload = BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    if (!game.IsRunning || sunshineGames.ContainsKey(game.Id)) continue;
                    SendCore(payload);
                    var delivered = Broadcast(payload, null);
                    if (delivered == 0) pendingGames.TryAdd(game.Id, 0);
                    sunshineGames.TryAdd(game.Id, 0);
                }
            }
        }

        private void SyncLauncher(PipeConnection launcher)
        {
            var running = RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true);
            var preferred = running.FirstOrDefault(x => SameId(launcher.GameId, x.Id));
            foreach (var game in preferred == null ? running : new[] { preferred })
            {
                var payload = BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    if (!game.IsRunning) continue;
                    var queued = launcher.Send(payload);
                    sunshineGames.TryAdd(game.Id, 0);
                    byte ignored;
                    if (queued) pendingGames.TryRemove(game.Id, out ignored);
                }
            }
        }

        private void FlushPending(PipeConnection launcher)
        {
            foreach (var gameId in pendingGames.Keys.ToArray())
            {
                var game = RunOnUi(() => api.Database.Games.Get(gameId), true);
                var payload = game == null || !game.IsRunning ? null : BuildStatus("gameStarted", game);
                lock (gameStateLock)
                {
                    byte ignored;
                    if (!pendingGames.ContainsKey(gameId)) continue;
                    if (game == null || !game.IsRunning)
                    {
                        pendingGames.TryRemove(gameId, out ignored);
                        continue;
                    }
                    if (launcher.Send(payload)) pendingGames.TryRemove(gameId, out ignored);
                }
            }
        }

        private void SendShutdownHandoff()
        {
            var running = RunOnUi(() => api.Database.Games.Where(x => x.IsRunning).ToArray(), true);
            if (running.Length != 0)
            {
                foreach (var game in running) Broadcast(BuildStatus("gameStarted", game), null);
            }
            else
            {
                Broadcast(Serialize(new Dictionary<string, object>
                {
                    ["type"] = "status", ["status"] = new Dictionary<string, object> { ["name"] = "playniteExiting" }
                }), null);
            }
        }

        private void FlushConnections(int timeoutMs)
        {
            var connections = new HashSet<PipeConnection>(launchers.Values);
            var currentCore = GetCore();
            if (currentCore != null) connections.Add(currentCore);
            var elapsed = Stopwatch.StartNew();
            foreach (var connection in connections)
            {
                var remaining = timeoutMs - (int)elapsed.ElapsedMilliseconds;
                if (remaining <= 0 || !connection.Flush(remaining)) break;
            }
        }

        private void SendCore(object message)
        {
            var target = GetCore();
            if (target != null) target.Send(message as string ?? Serialize(message));
        }

        private int Broadcast(string payload, IEnumerable<PipeConnection> targets)
        {
            var count = 0;
            foreach (var target in targets ?? launchers.Values)
            {
                if (target.Send(payload)) count++;
            }
            return count;
        }

        private PipeConnection GetCore() { lock (coreLock) return core; }

        private void ReplaceCore(PipeConnection replacement)
        {
            PipeConnection previous;
            lock (coreLock) { previous = core; core = replacement; }
            if (previous != null && !ReferenceEquals(previous, replacement)) previous.Dispose();
        }

        private Dictionary<string, object> ParseObject(string value)
        {
            return json.Deserialize<Dictionary<string, object>>(value) ?? new Dictionary<string, object>();
        }

        private string Serialize(object value) { lock (json) return json.Serialize(value); }

        private static string GetString(IDictionary<string, object> value, string key)
        {
            object item;
            return value != null && value.TryGetValue(key, out item) && item != null ? Convert.ToString(item) : string.Empty;
        }

        private static int? GetInt(IDictionary<string, object> value, string key)
        {
            object item;
            if (value == null || !value.TryGetValue(key, out item) || item == null) return null;
            try { return Convert.ToInt32(item); } catch { return null; }
        }

        private static IDictionary<string, string> GetEnvironment(IDictionary<string, object> message)
        {
            var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            object raw;
            var environment = message != null && message.TryGetValue("env", out raw) ? raw as Dictionary<string, object> : null;
            if (environment == null) return result;
            foreach (var pair in environment)
            {
                if (string.IsNullOrWhiteSpace(pair.Key) || pair.Key.IndexOfAny(new[] { '=', '\0' }) >= 0) continue;
                var value = pair.Value == null ? string.Empty : Convert.ToString(pair.Value);
                if (value.IndexOf('\0') < 0) result[pair.Key] = value;
            }
            return result;
        }

        private T RunOnUi<T>(Func<T> action, bool synchronous)
        {
            var dispatcher = api.MainView.UIDispatcher;
            if (dispatcher == null || dispatcher.CheckAccess()) return action();
            return dispatcher.Invoke(action);
        }

        private void RunOnUi(Action action, bool synchronous)
        {
            var dispatcher = api.MainView.UIDispatcher;
            if (dispatcher == null || dispatcher.CheckAccess()) action();
            else if (synchronous) dispatcher.Invoke(action);
            else dispatcher.BeginInvoke(action);
        }

        private static bool SameId(string left, object right)
        {
            if (string.IsNullOrWhiteSpace(left) || right == null) return false;
            return string.Equals(left.Trim().Trim('{', '}'), Convert.ToString(right).Trim().Trim('{', '}'), StringComparison.OrdinalIgnoreCase);
        }

        private sealed class Snapshot
        {
            public Snapshot(List<Dictionary<string, object>> categories, List<Dictionary<string, object>> plugins, List<Dictionary<string, object>> games)
            { Categories = categories; Plugins = plugins; Games = games; }
            public List<Dictionary<string, object>> Categories { get; private set; }
            public List<Dictionary<string, object>> Plugins { get; private set; }
            public List<Dictionary<string, object>> Games { get; private set; }
        }

        private sealed class ActionInfo
        {
            public string Path, Arguments, WorkingDirectory;
            public GameAction Source;
        }
    }

    internal sealed class PipeConnection : IDisposable
    {
        private readonly BlockingCollection<OutboundMessage> outbox = new BlockingCollection<OutboundMessage>();
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private readonly AutoResetEvent writeProgress = new AutoResetEvent(false);
        private readonly object sendLock = new object();
        private readonly ConnectorLog log;
        private readonly Task writerTask;
        private long nextSequence;
        private long completedSequence;
        private int disposed;

        public PipeConnection(string id, NamedPipeServerStream stream, StreamReader reader, StreamWriter writer, ConnectorLog log)
        {
            Id = id; Stream = stream; Reader = reader; Writer = writer; this.log = log;
            writerTask = Task.Factory.StartNew(WriteLoop, cancellation.Token, TaskCreationOptions.LongRunning, TaskScheduler.Default);
        }

        public string Id { get; private set; }
        public int? Pid { get; set; }
        public string GameId { get; set; }
        public NamedPipeServerStream Stream { get; private set; }
        public StreamReader Reader { get; private set; }
        public StreamWriter Writer { get; private set; }

        public bool Send(string payload)
        {
            return SendBatch(new[] { payload });
        }

        public bool SendBatch(IEnumerable<string> payloads)
        {
            if (payloads == null) return false;
            lock (sendLock)
            {
                if (Volatile.Read(ref disposed) != 0) return false;
                try
                {
                    foreach (var payload in payloads)
                    {
                        var sequence = nextSequence + 1;
                        outbox.Add(new OutboundMessage(sequence, payload));
                        Volatile.Write(ref nextSequence, sequence);
                    }
                    return true;
                }
                catch { return false; }
            }
        }

        public bool Flush(int timeoutMs)
        {
            var target = Volatile.Read(ref nextSequence);
            if (Volatile.Read(ref completedSequence) >= target) return true;
            var elapsed = Stopwatch.StartNew();
            try
            {
                while (Volatile.Read(ref disposed) == 0 && Volatile.Read(ref completedSequence) < target)
                {
                    var remaining = timeoutMs - (int)elapsed.ElapsedMilliseconds;
                    if (remaining <= 0 || !writeProgress.WaitOne(remaining)) return false;
                }
                return Volatile.Read(ref completedSequence) >= target;
            }
            catch (ObjectDisposedException) { return false; }
        }

        private void WriteLoop()
        {
            try
            {
                while (!cancellation.IsCancellationRequested)
                {
                    OutboundMessage message;
                    if (!outbox.TryTake(out message, 500)) continue;
                    Writer.WriteLine(message.Payload);
                    Writer.Flush();
                    Volatile.Write(ref completedSequence, message.Sequence);
                    writeProgress.Set();
                }
            }
            catch (Exception ex) { if (!cancellation.IsCancellationRequested) log.Debug("Pipe writer stopped: " + ex.Message); }
            finally { try { writeProgress.Set(); } catch { } }
        }

        public void Dispose()
        {
            lock (sendLock)
            {
                if (Interlocked.Exchange(ref disposed, 1) != 0) return;
                try { outbox.CompleteAdding(); } catch { }
            }
            try { writeProgress.Set(); } catch { }
            try { cancellation.Cancel(); } catch { }
            try { Stream.Dispose(); } catch { }
            try { writerTask.Wait(500); } catch { }
            try { Reader.Dispose(); } catch { }
            try { Writer.Dispose(); } catch { }
            try { outbox.Dispose(); } catch { }
            try { writeProgress.Dispose(); } catch { }
            try { cancellation.Dispose(); } catch { }
        }

        private sealed class OutboundMessage
        {
            public OutboundMessage(long sequence, string payload)
            {
                Sequence = sequence;
                Payload = payload;
            }

            public long Sequence { get; private set; }
            public string Payload { get; private set; }
        }
    }

    internal sealed class EnvironmentScopes
    {
        private readonly object sync = new object();
        private readonly Dictionary<string, Dictionary<string, string>> scopes = new Dictionary<string, Dictionary<string, string>>();
        private readonly Dictionary<string, string> originals = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        private readonly List<string> order = new List<string>();

        public void Set(string id, IDictionary<string, string> values)
        {
            lock (sync)
            {
                Dictionary<string, string> old;
                scopes.TryGetValue(id, out old);
                var affected = new HashSet<string>(values.Keys, StringComparer.OrdinalIgnoreCase);
                if (old != null) affected.UnionWith(old.Keys);
                foreach (var key in values.Keys) if (!originals.ContainsKey(key)) originals[key] = Environment.GetEnvironmentVariable(key);
                scopes[id] = new Dictionary<string, string>(values, StringComparer.OrdinalIgnoreCase);
                if (!order.Contains(id)) order.Add(id);
                Reconcile(affected);
            }
        }

        public void Clear(string id)
        {
            lock (sync)
            {
                Dictionary<string, string> old;
                if (!scopes.TryGetValue(id, out old)) return;
                scopes.Remove(id); order.Remove(id); Reconcile(old.Keys);
            }
        }

        public void RunTemporary(IDictionary<string, string> values, Action action)
        {
            lock (sync)
            {
                var previous = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                try
                {
                    foreach (var item in values) { previous[item.Key] = Environment.GetEnvironmentVariable(item.Key); Environment.SetEnvironmentVariable(item.Key, item.Value); }
                    action();
                }
                finally { foreach (var item in previous) Environment.SetEnvironmentVariable(item.Key, item.Value); }
            }
        }

        private void Reconcile(IEnumerable<string> keys)
        {
            foreach (var key in keys.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                var found = false; string value = null;
                for (var index = order.Count - 1; index >= 0; index--)
                {
                    if (scopes[order[index]].TryGetValue(key, out value)) { found = true; break; }
                }
                if (found) Environment.SetEnvironmentVariable(key, value);
                else if (originals.TryGetValue(key, out value)) { Environment.SetEnvironmentVariable(key, value); originals.Remove(key); }
            }
        }
    }
}
