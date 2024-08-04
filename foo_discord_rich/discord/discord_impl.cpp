#include <stdafx.h>

#include "discord_impl.h"

#include <album_art/album_art_fetcher.h>
#include <fb2k/config.h>
#include <fb2k/current_track_titleformat.h>

#include <ctime>

namespace drp::internal
{

PresenceData::PresenceData()
{
    memset( &presence, 0, sizeof( presence ) );
    presence.activityType = DiscordActivityType::LISTENING;
    presence.state = state.c_str();
    presence.details = details.c_str();
}

PresenceData::PresenceData( const PresenceData& other )
{
    CopyData( other );
}

PresenceData& PresenceData::operator=( const PresenceData& other )
{
    if ( this != &other )
    {
        CopyData( other );
    }

    return *this;
}

bool PresenceData::operator==( const PresenceData& other )
{
    auto areStringsSame = []( const char* a, const char* b ) {
        return ( ( a == b ) || ( a && b && !strcmp( a, b ) ) );
    };

    return ( areStringsSame( presence.state, other.presence.state ) && areStringsSame( presence.details, other.presence.details ) && areStringsSame( presence.largeImageKey, other.presence.largeImageKey ) && areStringsSame( presence.largeImageText, other.presence.largeImageText ) && areStringsSame( presence.smallImageKey, other.presence.smallImageKey ) && areStringsSame( presence.smallImageText, other.presence.smallImageText ) && presence.startTimestamp == other.presence.startTimestamp && presence.endTimestamp == other.presence.endTimestamp && trackLength == other.trackLength );
}

bool PresenceData::operator!=( const PresenceData& other )
{
    return !operator==( other );
}

void PresenceData::CopyData( const PresenceData& other )
{
    metadb = other.metadb;
    state = other.state;
    details = other.details;
    largeImageKey = other.largeImageKey;
    smallImageKey = other.smallImageKey;
    trackLength = other.trackLength;

    memcpy( &presence, &other.presence, sizeof( presence ) );
    presence.activityType = DiscordActivityType::LISTENING;
    presence.state = state.c_str();
    presence.details = details.c_str();
    presence.largeImageKey = ( largeImageKey.empty() ? nullptr : largeImageKey.c_str() );
    presence.smallImageKey = ( smallImageKey.empty() ? nullptr : smallImageKey.c_str() );
}

} // namespace drp::internal

namespace drp
{

PresenceModifier::PresenceModifier( DiscordHandler& parent, const drp::internal::PresenceData& presenceData )
    : parent_( parent )
    , presenceData_( presenceData )
{
}

PresenceModifier::~PresenceModifier()
{
    const bool hasChanged = ( parent_.presenceData_ != presenceData_ );
    if ( hasChanged )
    {
        parent_.presenceData_ = presenceData_;
    }

    const bool needsToBeDisabled = ( isDisabled_ || !playback_control::get()->is_playing() || ( playback_control::get()->is_paused() && config::disableWhenPaused ) );
    if ( needsToBeDisabled )
    {
        if ( parent_.HasPresence() )
        {
            parent_.ClearPresence();
        }
    }
    else
    {
        if ( !parent_.HasPresence() || hasChanged )
        {
            parent_.SendPresence();
        }
    }
}

void PresenceModifier::UpdateImage( metadb_handle_ptr metadb )
{
    auto& pd = presenceData_;
    auto pc = playback_control::get();

    auto setImageKey = [&pd]( const qwr::u8string& imageKey ) {
        pd.largeImageKey = imageKey;
        pd.presence.largeImageKey = pd.largeImageKey.empty() ? nullptr : pd.largeImageKey.c_str();
    };

    // TODO: fix query
    const auto userReleaseMbid = EvaluateQueryForCurrentTrack( metadb, "$meta(MUSICBRAINZ ALBUM ID)" );
    const AlbumArtFetcher::FetchRequest request{
        .artist = EvaluateQueryForCurrentTrack( metadb, "%artist%" ),
        .album = EvaluateQueryForCurrentTrack( metadb, "%album%" ),
        .userReleaseMbidOpt = userReleaseMbid.empty() ? std::optional<qwr::u8string>{} : userReleaseMbid
    };

    // TODO: add proper config (make MBID search optional as well)
    const auto artUrlOpt = AlbumArtFetcher::Get().GetArtUrl( request );
    if ( artUrlOpt )
    {
        setImageKey( *artUrlOpt );
    }
    else
    {
        switch ( config::largeImageSettings )
        {
        case config::ImageSetting::Light:
        {
            setImageKey( config::largeImageId_Light );
            break;
        }
        case config::ImageSetting::Dark:
        {
            setImageKey( config::largeImageId_Dark );
            break;
        }
        case config::ImageSetting::Disabled:
        {
            setImageKey( qwr::u8string{} );
            break;
        }
        }
    }
}

void PresenceModifier::UpdateSmallImage()
{
    auto& pd = presenceData_;
    auto pc = playback_control::get();

    auto setImageKey = [&pd]( const qwr::u8string& imageKey ) {
        pd.smallImageKey = imageKey;
        pd.presence.smallImageKey = pd.smallImageKey.empty() ? nullptr : pd.smallImageKey.c_str();
    };

    const bool usePausedImage = ( pc->is_paused() || config::swapSmallImages );

    switch ( config::smallImageSettings )
    {
    case config::ImageSetting::Light:
    {
        setImageKey( usePausedImage ? config::pausedImageId_Light : config::playingImageId_Light );
        break;
    }
    case config::ImageSetting::Dark:
    {
        setImageKey( usePausedImage ? config::pausedImageId_Dark : config::playingImageId_Dark );
        break;
    }
    case config::ImageSetting::Disabled:
    {
        setImageKey( qwr::u8string{} );
        break;
    }
    }
}

void PresenceModifier::UpdateTrack( metadb_handle_ptr metadb )
{
    auto& pd = presenceData_;

    pd.state.clear();
    pd.details.clear();
    pd.trackLength = 0;

    if ( metadb.is_valid() )
    { // Need to save, since refresh might be required when settings are changed
        pd.metadb = metadb;
    }

    const auto queryData = [metadb = pd.metadb]( const qwr::u8string& query ) {
        return EvaluateQueryForCurrentTrack( metadb, query );
    };
    const auto fixStringLength = []( qwr::u8string& str ) {
        if ( str.length() == 1 )
        { // minimum allowed non-zero string length is 2, so we need to pad it
            str += ' ';
        }
        else if ( str.length() > 127 )
        { // maximum allowed length is 127
            str.resize( 127 );
        }
    };

    pd.state = queryData( config::stateQuery );
    fixStringLength( pd.state );
    pd.details = queryData( config::detailsQuery );
    fixStringLength( pd.details );

    const qwr::u8string lengthStr = queryData( "[%length_seconds_fp%]" );
    pd.trackLength = ( lengthStr.empty() ? 0 : stold( lengthStr ) );

    const qwr::u8string durationStr = queryData( "[%playback_time_seconds%]" );

    pd.presence.state = pd.state.c_str();
    pd.presence.details = pd.details.c_str();
    UpdateDuration( durationStr.empty() ? 0 : stold( durationStr ) );
    UpdateImage( metadb );
}

void PresenceModifier::UpdateDuration( double time )
{
    auto& pd = presenceData_;
    auto pc = playback_control::get();
    const config::TimeSetting timeSetting = ( ( pd.trackLength && pc->is_playing() && !pc->is_paused() ) ? config::timeSettings : config::TimeSetting::Disabled );
    switch ( timeSetting )
    {
    case config::TimeSetting::Elapsed:
    {
        pd.presence.startTimestamp = std::time( nullptr ) - std::llround( time );
        pd.presence.endTimestamp = 0;

        break;
    }
    case config::TimeSetting::Remaining:
    {
        pd.presence.startTimestamp = 0;
        pd.presence.endTimestamp = std::time( nullptr ) + std::max<uint64_t>( 0, std::llround( pd.trackLength - time ) );

        break;
    }
    case config::TimeSetting::Disabled:
    {
        pd.presence.startTimestamp = 0;
        pd.presence.endTimestamp = 0;

        break;
    }
    }
}

void PresenceModifier::DisableDuration()
{
    auto& pd = presenceData_;
    pd.presence.startTimestamp = 0;
    pd.presence.endTimestamp = 0;
}

void PresenceModifier::Disable()
{
    isDisabled_ = true;
}

DiscordHandler& DiscordHandler::GetInstance()
{
    static DiscordHandler discordHandler;
    return discordHandler;
}

void DiscordHandler::Initialize()
{
    appToken_ = config::discordAppToken;

    DiscordEventHandlers handlers{};

    handlers.ready = OnReady;
    handlers.disconnected = OnDisconnected;
    handlers.errored = OnErrored;

    Discord_Initialize( appToken_.c_str(), &handlers, 1, nullptr );
    Discord_RunCallbacks();

    hasPresence_ = true; ///< Discord may use default app handler, which we need to override

    auto pm = GetPresenceModifier();
    pm.UpdateImage();
    pm.Disable(); ///< we don't want to activate presence yet
}

void DiscordHandler::Finalize()
{
    Discord_ClearPresence();
    Discord_Shutdown();
}

void DiscordHandler::OnSettingsChanged()
{
    if ( appToken_ != static_cast<std::string>( config::discordAppToken ) )
    {
        Finalize();
        Initialize();
    }

    auto pm = GetPresenceModifier();
    pm.UpdateImage();
    pm.UpdateSmallImage();
    pm.UpdateTrack();
    if ( !config::isEnabled )
    {
        pm.Disable();
    }
}

void DiscordHandler::OnImageLoaded()
{
    auto pm = GetPresenceModifier();
    pm.UpdateImage();
}

bool DiscordHandler::HasPresence() const
{
    return hasPresence_;
}

void DiscordHandler::SendPresence()
{
    if ( config::isEnabled )
    {
        Discord_UpdatePresence( &presenceData_.presence );
        hasPresence_ = true;
    }
    else
    {
        Discord_ClearPresence();
        hasPresence_ = false;
    }
    Discord_RunCallbacks();
}

void DiscordHandler::ClearPresence()
{
    Discord_ClearPresence();
    hasPresence_ = false;

    Discord_RunCallbacks();
}

PresenceModifier DiscordHandler::GetPresenceModifier()
{
    return PresenceModifier( *this, presenceData_ );
}

void DiscordHandler::OnReady( const DiscordUser* request )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": connected to " << ( request->username ? request->username : "<null>" );
}

void DiscordHandler::OnDisconnected( int errorCode, const char* message )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": disconnected with code " << errorCode;
    if ( message )
    {
        FB2K_console_formatter() << message;
    }
}

void DiscordHandler::OnErrored( int errorCode, const char* message )
{
    FB2K_console_formatter() << DRP_NAME_WITH_VERSION << ": error " << errorCode;
    if ( message )
    {
        FB2K_console_formatter() << message;
    }
}

} // namespace drp
