//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  ishani.org 2022              e.t.c.                  MIT License
//
//  
//

#pragma once

namespace config {

// ---------------------------------------------------------------------------------------------------------------------
struct IPathProvider
{
    static constexpr auto cOuroveonRootName = "OUROVEON";

    enum class PathFor
    {
        SharedConfig,       // directory for storing shared core configuration (eg. audio driver choice)
        SharedData,         // shared data directory root common for all apps (eg. fonts)
        PerAppConfig,       // directory for storing per-app data (eg. current window state, app-specific blobs)
    };

    virtual fs::path getPath( const PathFor p ) const = 0;
};

// ---------------------------------------------------------------------------------------------------------------------

template<typename T>
concept Serializeable = requires( T& t )
{
    { T::StoragePath }        -> std::convertible_to<IPathProvider::PathFor>;
    { T::StorageFilename }    -> std::convertible_to<std::string>;
};

/*
template<typename T>
constexpr bool HasPostLoad = requires( T& t )
{
    { t.postLoad() } -> std::convertible_to<bool>;
};

template<typename T>
constexpr bool HasPreSave = requires( const T& t )
{
    { t.preSave() } -> std::convertible_to<bool>;
};
*/

struct Base
{
    virtual ~Base() {}
    virtual bool postLoad() { return true; }
    virtual bool preSave() { return true; }
};


// ---------------------------------------------------------------------------------------------------------------------
enum class LoadResult
{
    Success,
    CannotFindConfigFile,
    ErrorDuringLoad,
    FailedPostLoad
};

enum class SaveResult
{
    Success,
    FailedPreSave,
    Failed
};

// ---------------------------------------------------------------------------------------------------------------------
// return the full assembled path for the given config type
//
template < Serializeable _config_type >
inline fs::path getFullPath( const IPathProvider& pathProvider )
{
    fs::path loadPath = pathProvider.getPath( _config_type::StoragePath );
    loadPath.append( _config_type::StorageFilename );
    return loadPath;
}

// ---------------------------------------------------------------------------------------------------------------------
// generic load & save functions for a Base derived data type

template < Serializeable _config_type >
inline LoadResult load( const IPathProvider& pathProvider, _config_type& result )
{
    const fs::path loadPath = getFullPath<_config_type>( pathProvider );

    if ( !fs::exists( loadPath ) )
        return LoadResult::CannotFindConfigFile;

    try
    {
        std::ifstream is( loadPath );
        cereal::JSONInputArchive archive( is );

        result.serialize( archive );
    }
    catch ( cereal::Exception& cEx )
    {
        blog::error::cfg( "cannot parse [{}] : {}", loadPath.string(), cEx.what() );
        return LoadResult::ErrorDuringLoad;
    }

    //if constexpr ( HasPostLoad<_config_type> )
    {
        if ( !result.postLoad() )
        {
            return LoadResult::FailedPostLoad;
        }
    }

    return LoadResult::Success;
}

template < Serializeable _config_type >
inline LoadResult loadFromMemory( const std::string& rawJson, _config_type& result )
{
    try
    {
        std::istringstream is( rawJson );
        cereal::JSONInputArchive archive( is );

        result.serialize( archive );
    }
    catch ( cereal::Exception& cEx )
    {
        blog::error::cfg( "cannot parse from JSON : {}", cEx.what() );
        return LoadResult::ErrorDuringLoad;
    }

    //if constexpr ( HasPostLoad<_config_type> )
    {
        if ( !result.postLoad() )
        {
            return LoadResult::FailedPostLoad;
        }
    }

    return LoadResult::Success;

}

template < Serializeable _config_type >
inline SaveResult save( const IPathProvider& pathProvider, _config_type& data )
{
    //if constexpr ( HasPreSave<_config_type> )
    {
        if ( !data.preSave() )
        {
            return SaveResult::FailedPreSave;
        }
    }

    const fs::path savePath = getFullPath<_config_type>( pathProvider );

    try
    {
        std::ofstream is( savePath );
        cereal::JSONOutputArchive archive( is );

        data.serialize( archive );
    }
    catch ( cereal::Exception& cEx )
    {
        blog::error::cfg( "cannot write [{}] | {}", savePath.string(), cEx.what() );
        return SaveResult::Failed;
    }

    return SaveResult::Success;
}

} // namespace config
