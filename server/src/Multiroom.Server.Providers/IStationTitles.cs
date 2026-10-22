namespace Multiroom.Server.Providers;

// A provider whose live stations announce what they play (ICY StreamTitle):
// webradio and TuneIn. For the UI only: the rooms keep the station name.
public interface IStationTitles
{
    // The title the station last announced while a relay of it is open.
    string? CurrentTitle(string stationItemId);
}
