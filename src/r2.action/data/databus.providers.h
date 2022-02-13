//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  ishani.org 2022              e.t.c.                  MIT License
//
//  
//

#pragma once

namespace data {

// ---------------------------------------------------------------------------------------------------------------------
struct Provider
{
    enum AbilityFlags
    {
        kNothingSpecial = 0,
        kUsesValue      = 1 << 0,
        kUsesTime       = 1 << 1,
        kUsesBus1       = 1 << 2,
        kUsesBus2       = 1 << 3,
        kUsesRemapping  = 1 << 4
    };
    struct Input
    {
        float       m_value = 0;
        float       m_time  = 0;
        float       m_bus1  = -1;
        float       m_bus2  = -1;
    };

    virtual AbilityFlags flags() const { return kNothingSpecial; }
    virtual float generate( const Input& input ) = 0;
};

using ProviderFactory = std::unordered_map< uint32_t, std::function< Provider*() > >;
using ProviderNames   = std::unordered_map< uint32_t, std::string >;

// ---------------------------------------------------------------------------------------------------------------------
namespace providers {

#define PROVIDER_ID(ch0, ch1, ch2, ch3)         \
                ((uint32_t)(BYTE)(ch0)        | \
                ((uint32_t)(BYTE)(ch1) << 8)  | \
                ((uint32_t)(BYTE)(ch2) << 16) | \
                ((uint32_t)(BYTE)(ch3) << 24  ))

template< typename _type >
inline static void registerProvider( ProviderFactory& factory, ProviderNames& names )
{
    factory.try_emplace( _type::UniqueID, []() { return new _type(); } );
    names.try_emplace(   _type::UniqueID, _type::VisibleName );
}

template< typename _type, typename _customFactory >
inline static void registerProvider( ProviderFactory& factory, ProviderNames& names, const _customFactory cfn )
{
    factory.try_emplace( _type::UniqueID, cfn );
    names.try_emplace( _type::UniqueID, _type::VisibleName );
}


// ---------------------------------------------------------------------------------------------------------------------
struct SinTime : public Provider
{
    static constexpr size_t UniqueID         = PROVIDER_ID( 'S', 'I', 'N', 'T' );
    static constexpr const char* VisibleName = "Sin( time )";

    virtual AbilityFlags flags() const { return (AbilityFlags)( kUsesTime | kUsesRemapping ); }
    virtual float generate( const Input& input ) override;
};

// ---------------------------------------------------------------------------------------------------------------------
struct CosBus : public Provider
{
    static constexpr size_t UniqueID         = PROVIDER_ID( 'C', 'O', 'S', 'B' );
    static constexpr const char* VisibleName = "Cos( bus-1 )";

    virtual AbilityFlags flags() const { return (AbilityFlags)( kUsesValue | kUsesBus1 | kUsesRemapping ); }
    virtual float generate( const Input& input ) override;
};

// ---------------------------------------------------------------------------------------------------------------------
struct Multiply2 : public Provider
{
    static constexpr size_t UniqueID         = PROVIDER_ID( 'M', 'U', 'L', '2' );
    static constexpr const char* VisibleName = "Multiply 2";

    virtual AbilityFlags flags() const { return (AbilityFlags)( kUsesBus1 | kUsesBus2 | kUsesRemapping ); }
    virtual float generate( const Input& input ) override;
};

// ---------------------------------------------------------------------------------------------------------------------
void registerDefaults( ProviderFactory& factory, ProviderNames& names );

} // namespace providers
} // namespace data