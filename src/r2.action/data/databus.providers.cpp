//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  ishani.org 2022              e.t.c.                  MIT License
//
//  
//

#include "pch.h"

#include "data/databus.providers.h"

namespace data {
namespace providers {

void registerDefaults( ProviderFactory& factory, ProviderNames& names )
{
    registerProvider<SinTime>( factory, names );
    registerProvider<CosBus>( factory, names );
    registerProvider<Multiply2>( factory, names );
}

float SinTime::generate( const Input& input )
{
    return ( 1.0f + std::sin( input.m_time ) ) * 0.5f;
}

float CosBus::generate( const Input& input )
{
    if ( input.m_bus1 < 0 )
        return 0.0f;

    return ( 1.0f + std::cos( input.m_bus1 * constants::f_2pi * input.m_value ) ) * 0.5f;
}

float Multiply2::generate( const Input& input )
{
    return ( input.m_bus1 * input.m_bus2 );
}

} // namespace providers
} // namespace data