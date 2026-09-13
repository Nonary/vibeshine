using System;
using Playnite.SDK;
using Playnite.SDK.Events;
using Playnite.SDK.Models;
using Playnite.SDK.Plugins;

namespace SunshinePlaynite
{
    public sealed class SunshinePlaynitePlugin : GenericPlugin
    {
        private readonly ConnectorService connector;

        public override Guid Id { get; } = Guid.Parse("E9B40F2D-8EED-4B5C-9149-D780E2F1268D");

        public SunshinePlaynitePlugin(IPlayniteAPI api) : base(api)
        {
            Properties = new GenericPluginProperties { HasSettings = false };
            connector = new ConnectorService(api, LogManager.GetLogger());
        }

        public override void OnApplicationStarted(OnApplicationStartedEventArgs args)
        {
            connector.Start();
        }

        public override void OnApplicationStopped(OnApplicationStoppedEventArgs args)
        {
            connector.Stop();
        }

        public override void OnGameStarted(OnGameStartedEventArgs args)
        {
            connector.GameStarted(args.Game);
        }

        public override void OnGameStopped(OnGameStoppedEventArgs args)
        {
            connector.GameStopped(args.Game);
        }

        public override void OnLibraryUpdated(OnLibraryUpdatedEventArgs args)
        {
            connector.QueueSnapshot();
        }

        public override void Dispose()
        {
            connector.Dispose();
            base.Dispose();
        }
    }
}
