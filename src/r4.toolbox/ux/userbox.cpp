//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  ishani.org 2022              e.t.c.                  MIT License
//

#include "pch.h"

#include "ux/userbox.h"
#include "endlesss/toolkit.exchange.h"

namespace ImGui {
namespace ux {

void UserBox::imgui( const endlesss::toolkit::PopulationQuery& population, float itemWidth )
{
    if ( itemWidth > 0 )
        ImGui::SetNextItemWidth( itemWidth );

    const bool bIsInputEnterPressed  = ImGui::InputText( "##username_entry", &m_username, ImGuiInputTextFlags_EnterReturnsTrue );
    const bool bIsInputActive        = ImGui::IsItemActive();
    const bool bIsInputJustActivated = ImGui::IsItemActivated();

    const ImGuiWindowFlags popupWindowFlags = 0
        | ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_ChildWindow
        ;

    // activate popup when text input first gets activated
    if ( bIsInputJustActivated )
    {
        ImGui::OpenPopup( "##username_autocomplete" );
    }

    // allow up/down navigation on arrow keys
    if ( bIsInputActive )
    {
        if ( ImGui::IsKeyPressedMap( ImGuiKey_DownArrow, false ) )
        {
            if ( m_suggestionIndex == -1 )
                m_suggestionIndex = 0;
            else
                m_suggestionIndex++;
        }
        if ( ImGui::IsKeyPressedMap( ImGuiKey_UpArrow, false ) )
        {
            if ( m_suggestionIndex == -1 )
                m_suggestionIndex = 0;
            else
                m_suggestionIndex--;
        }
    }

    {
        // configure the pop-up location and shape
        ImGui::SetNextWindowPos( { ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y } );
        ImGui::SetNextWindowSize( { ImGui::GetItemRectSize().x, 0 } );

        // has user typed anything?
        if ( !m_username.empty() )
        {
            // go see if we can offer name suggestions
            if ( population.prefixQuery( m_username, m_autocompletion ) )
            {
                if ( ImGui::BeginPopup( "##username_autocomplete", popupWindowFlags ) )
                {
                    // loop the suggestion index
                    if ( m_suggestionIndex < 0 )
                        m_suggestionIndex = static_cast<int32_t>( m_autocompletion.m_validCount ) - 1;
                    if ( m_suggestionIndex >= m_autocompletion.m_validCount )
                        m_suggestionIndex = 0;

                    // draw the suggestions
                    for ( auto suggestion = 0; suggestion < m_autocompletion.m_validCount; suggestion++ )
                    {
                        if ( ImGui::Selectable( m_autocompletion.m_values[suggestion].c_str(), suggestion == m_suggestionIndex ) )
                        {
                            ImGui::ClearActiveID();
                            m_username = m_autocompletion.m_values[suggestion];
                        }
                    }

                    // if user hits enter with a suggestion selected, copy it in
                    if ( bIsInputEnterPressed && m_suggestionIndex != -1 )
                    {
                        m_username = m_autocompletion.m_values[m_suggestionIndex];
                        m_suggestionIndex = -1;
                    }

                    // finish on enter or focus-away
                    if ( bIsInputEnterPressed || (!bIsInputActive && !ImGui::IsWindowFocused()) )
                        ImGui::CloseCurrentPopup();

                    ImGui::EndPopup();
                }
            }
        }
        else
        {
            m_suggestionIndex = 0;
        }
    }
}

} // namespace ux
} // name