using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Linq;
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
        private readonly IPlayniteAPI api;
        private readonly ConnectorLog log;
        private readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        private readonly object coreLock = new object();
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
            if (Interlocked.Exchange(ref started, 1) != 0) return;
            cancellation = new CancellationTokenSource();
            api.Database.Games.ItemCollectionChanged += GamesChanged;
            api.Database.Games.ItemUpdated += GamesUpdated;
            snapshotTimer = new Timer(_ => SendSnapshot(), null, Timeout.Infinite, Timeout.Infinite);
            serverTask = Task.Factory.StartNew(() => ServerLoop(cancellation.Token), cancellation.Token,
                TaskCreationOptions.LongRunning, TaskScheduler.Default);
            log.Info("Compiled Playnite plugin started");
        }

        public void Stop()
        {
            if (Interlocked.Exchange(ref started, 0) == 0) return;
            log.Info("Beginning shutdown");
            SendShutdownHandoff();
            Thread.Sleep(400);
            try { api.Database.Games.ItemCollectionChanged -= GamesChanged; } catch { }
            try { api.Database.Games.ItemUpdated -= GamesUpdated; } catch { }
            try { snapshotTimer.Dispose(); } catch { }
            try { cancellation.Cancel(); } catch { }
            try { pendingPipe.Dispose(); } catch { }
            ReplaceCore(null);
            foreach (var item in launchers.ToArray())
            {
                PipeConnection ignored;
                if (launchers.TryRemove(item.Key, out ignored)) ignored.Dispose();
                environmentScopes.Clear(item.Key);
            }
            try { serverTask.Wait(1000); } catch { }
            log.Info("Connector stopped");
        }

        public void Dispose()
        {
            Stop();
            log.Dispose();
            if (cancellation != null) cancellation.Dispose();
        }

        public void GameStarted(Game game)
        {
            if (game == null) return;
            log.Info("Game started: " + game.Name + " [" + game.Id + "]");
            if (launchers.Values.Any(x => SameId(x.GameId, game.Id))) sunshineGames.TryAdd(game.Id, 0);
            SendStatus("gameStarted", game);
        }

        public void GameStopped(Game game)
        {
            if (game == null) return;
            log.Info("Game stopped: " + game.Name + " [" + game.Id + "]");
            byte ignored;
            if (!sunshineGames.TryRemove(game.Id, out ignored))
            {
                log.Debug("Ignoring stop status for an untracked game: " + game.Id);
                return;
            }
            SendStatus("gameStopped", game);
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
                    WaitForConnection(data, token);

                    var reader = new StreamReader(data, new UTF8Encoding(false), false, 8192, true);
                    var writer = new StreamWriter(data, new UTF8Encoding(false), 8192, true) { AutoFlush = true };
                    var helloLine = reader.ReadLine();
                    if (helloLine == null) throw new IOException("No hello received");
                    var hello = ParseObject(helloLine);
                    var role = GetString(hello, "role");
                    if (string.IsNullOrEmpty(role)) role = GetCore() == null ? "sunshine" : "launcher";
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
            if (security != null)
            {
                return new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                    options, 65536, 65536, security);
            }
            return new NamedPipeServerStream(name, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                options, 65536, 65536);
        }

        private static PipeSecurity CreatePipeSecurity()
        {
            try
            {
                var security = new PipeSecurity();
                var allow = AccessControlType.Allow;
                security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null), PipeAccessRights.FullControl, allow));
                security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.InteractiveSid, null), PipeAccessRights.ReadWrite, allow));
                security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(WellKnownSidType.WorldSid, null), PipeAccessRights.ReadWrite, allow));
                var user = WindowsIdentity.GetCurrent().User;
                if (user != null) security.AddAccessRule(new PipeAccessRule(user, PipeAccessRights.FullControl, allow));
                return security;
            }
            catch { return null; }
        }

        private static void WaitForConnection(NamedPipeServerStream pipe, CancellationToken token)
        {
            var wait = pipe.WaitForConnectionAsync();
            while (!wait.Wait(200)) token.ThrowIfCancellationRequested();
            if (!pipe.IsConnected) throw new IOException("Pipe failed to connect");
        }

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
                sunshineGames.TryAdd(gameId, 0);
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
                            sunshineGames.TryAdd(gameId, 0);
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
            var target = GetCore();
            if (target == null) return;
            try
            {
                var snapshot = RunOnUi(BuildSnapshot, true);
                target.Send(Serialize(new Dictionary<string, object> { ["type"] = "snapshotStart" }));
                target.Send(Serialize(new Dictionary<string, object> { ["type"] = "plugins", ["payload"] = snapshot.Plugins }));
                target.Send(Serialize(new Dictionary<string, object> { ["type"] = "categories", ["payload"] = snapshot.Categories }));
                for (var index = 0; index < snapshot.Games.Count; index += 100)
                {
                    target.Send(Serialize(new Dictionary<string, object>
                    {
                        ["type"] = "games", ["payload"] = snapshot.Games.Skip(index).Take(100).ToArray()
                    }));
                }
                target.Send(Serialize(new Dictionary<string, object> { ["type"] = "snapshotComplete", ["games"] = snapshot.Games.Count }));
                log.Info(string.Format("Snapshot completed: categories={0} games={1}", snapshot.Categories.Count, snapshot.Games.Count));
            }
            catch (Exception ex) { log.Warn("Snapshot failed: " + ex.Message); }
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

        private void SendStatus(string name, Game game)
        {
            var payload = RunOnUi(() => BuildStatus(name, game), true);
            SendCore(payload);
            Broadcast(payload, null);
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
                if (!sunshineGames.ContainsKey(game.Id))
                {
                    var payload = BuildStatus("gameStarted", game);
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
                launcher.Send(BuildStatus("gameStarted", game));
                sunshineGames.TryAdd(game.Id, 0);
                byte ignored;
                pendingGames.TryRemove(game.Id, out ignored);
            }
        }

        private void FlushPending(PipeConnection launcher)
        {
            foreach (var gameId in pendingGames.Keys.ToArray())
            {
                var game = RunOnUi(() => api.Database.Games.Get(gameId), true);
                if (game != null) launcher.Send(BuildStatus("gameStarted", game));
                byte ignored;
                pendingGames.TryRemove(gameId, out ignored);
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
        private readonly BlockingCollection<string> outbox = new BlockingCollection<string>();
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private readonly ConnectorLog log;
        private readonly Task writerTask;
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
            if (Volatile.Read(ref disposed) != 0) return false;
            try { outbox.Add(payload); return true; } catch { return false; }
        }

        private void WriteLoop()
        {
            try
            {
                while (!cancellation.IsCancellationRequested)
                {
                    string line;
                    if (!outbox.TryTake(out line, 500)) continue;
                    Writer.WriteLine(line);
                    Writer.Flush();
                }
            }
            catch (Exception ex) { if (!cancellation.IsCancellationRequested) log.Debug("Pipe writer stopped: " + ex.Message); }
        }

        public void Dispose()
        {
            if (Interlocked.Exchange(ref disposed, 1) != 0) return;
            try { cancellation.Cancel(); } catch { }
            try { Stream.Dispose(); } catch { }
            try { writerTask.Wait(500); } catch { }
            try { Reader.Dispose(); } catch { }
            try { Writer.Dispose(); } catch { }
            outbox.Dispose();
            cancellation.Dispose();
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
            var previous = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                foreach (var item in values) { previous[item.Key] = Environment.GetEnvironmentVariable(item.Key); Environment.SetEnvironmentVariable(item.Key, item.Value); }
                action();
            }
            finally { foreach (var item in previous) Environment.SetEnvironmentVariable(item.Key, item.Value); }
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
