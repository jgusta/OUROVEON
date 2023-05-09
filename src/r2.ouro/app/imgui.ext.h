//   _______ _______ ______ _______ ___ ___ _______ _______ _______ 
//  |       |   |   |   __ \       |   |   |    ___|       |    |  |
//  |   -   |   |   |      <   -   |   |   |    ___|   -   |       |
//  |_______|_______|___|__|_______|\_____/|_______|_______|__|____|
//  ishani.org 2022              e.t.c.                  MIT License
//
//  imgui extras
//

#pragma once
#include "base/mathematics.h"


// ---------------------------------------------------------------------------------------------------------------------
inline ImVec4 lerpVec4( const ImVec4& from, const ImVec4& to, const float t )
{
    return ImVec4(
        base::lerp( from.x, to.x, t ),
        base::lerp( from.y, to.y, t ),
        base::lerp( from.z, to.z, t ),
        base::lerp( from.w, to.w, t )
    );
}

// ---------------------------------------------------------------------------------------------------------------------
namespace ImGui {

inline void MakeTabVisible( const char* window_name )
{
    ImGuiWindow* window = ImGui::FindWindowByName( window_name );
    if ( window == nullptr || window->DockNode == nullptr || window->DockNode->TabBar == nullptr )
        return;

    window->DockNode->TabBar->NextSelectedTabId = window->ID;
}

inline bool Shortcut( ImGuiKeyModFlags mod, ImGuiKey key, bool repeat )
{
	return mod == GetMergedKeyModFlags() && IsKeyPressed( GetKeyIndex(key), repeat );
}

template < typename _HandleType >
struct _ImTexture
{
    ImVec2          m_size;
    int32_t         m_width;
    int32_t         m_height;
    _HandleType     m_handle;
};

using ImTexture = _ImTexture<uint32_t>;
bool ImTextureFromFile( const char* filename, ImTexture& texture, bool clampToEdges );


inline void SeparatorBreak()
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
}
inline void ColumnSeparatorBreak()
{
    ImGui::Spacing();
    ImGui::ColumnSeparator();
    ImGui::Spacing();
}

void BeginDisabledControls( const bool isDisabled );
void EndDisabledControls( const bool isDisabled );

// ---------------------------------------------------------------------------------------------------------------------
ImU32 ParseHexColour( const char* hexColour );

// ---------------------------------------------------------------------------------------------------------------------
ImVec4 GetPulseColourVec4();
ImU32 GetPulseColour();

inline ImVec4 GetWarningTextColour() { return ImVec4( 0.981f, 0.874f, 0.378f, 0.985f ); }
inline ImVec4 GetErrorTextColour() { return ImVec4(0.981f, 0.074f, 0.178f, 0.985f); }

// ---------------------------------------------------------------------------------------------------------------------
void CompactTooltip( const char* tip );
void CompactTooltip( const std::string& tip );

// ---------------------------------------------------------------------------------------------------------------------
bool KnobFloat( const char* label, const float outerRadius, float* p_value, float v_min, float v_max, float v_step );

// ---------------------------------------------------------------------------------------------------------------------
bool Spinner( const char* label, bool active, float radius, float thickness, float yOffset, const ImU32& color );

// ---------------------------------------------------------------------------------------------------------------------
void BeatSegments(
    const char* label,
    const int numSegments,
    const int activeSegment,
    const int activeEdge = -1,
    const float height = 5.0f,
    const ImU32 highlightColour = ImGui::GetColorU32( ImGuiCol_TextDisabled ),
    const ImU32 backgroundColour = ImGui::GetColorU32( ImGuiCol_FrameBg ) );

// ---------------------------------------------------------------------------------------------------------------------
void VerticalProgress(
    const char* label,
    const float progress,
    const ImU32 highlightColour = ImGui::GetColorU32( ImGuiCol_Text ) );

// ---------------------------------------------------------------------------------------------------------------------
void RenderArrowSolid( ImDrawList* draw_list, ImVec2 pos, ImU32 col, ImGuiDir dir, float height );

// ---------------------------------------------------------------------------------------------------------------------
bool ClickableText( const char* label );

void CenteredText( const char* text );
void CenteredColouredText( const ImVec4& col, const char* text );


template< typename _T, int32_t _Size >
struct RingBufferedGraph
{
    _T      data[_Size];
    int32_t writeIndex = 0;

    RingBufferedGraph( const _T defaults )
    {
        for ( auto i = 0; i < _Size; i++ )
            data[i] = defaults;
    }

    void append( const _T v )
    {
        data[writeIndex] = v;
        writeIndex++;
        if ( writeIndex > _Size )
            writeIndex = 0;
    }

    void imgui( const char* label )
    {
        ImPlot::PlotBars( label, data, _Size, 0.6, 0, writeIndex );
    }
};

namespace Scoped {

void TickPulses();

// ---------------------------------------------------------------------------------------------------------------------
struct ButtonTextAlignLeft
{
    ButtonTextAlignLeft();
    ~ButtonTextAlignLeft();
private:
    ImVec2  m_state;
};

// ---------------------------------------------------------------------------------------------------------------------
struct ToggleButton
{
    ToggleButton( bool state, bool pulse = false );
    ~ToggleButton();

private:
    bool    m_state;
};


// ---------------------------------------------------------------------------------------------------------------------
struct FluxButton
{
    enum class State
    {
        Off,
        Flux,
        On,
    };

    FluxButton( const State& state, const ImVec4& buttonColourOn, const ImVec4& textColourOn );
    ~FluxButton();

private:
    int32_t     m_stylesToPop;
};


// ---------------------------------------------------------------------------------------------------------------------
struct ToggleButtonLit
{
    ToggleButtonLit( const bool active, const uint32_t colourU32 );
    ~ToggleButtonLit();

private:
    bool    m_active;
};

// ---------------------------------------------------------------------------------------------------------------------
struct ToggleTab
{
    ToggleTab( bool state, bool pulse = false );
    ~ToggleTab();

private:
    bool    m_state;
};

// ---------------------------------------------------------------------------------------------------------------------
struct FloatTextRight
{
    FloatTextRight( const char* text );
    ~FloatTextRight();

private:
    ImVec2  m_savedCursor;
};

} // namespace Scoped



// ---------------------------------------------------------------------------------------------------------------------
// some ImGui helpers for displaying combos of values|labels where the selection is driven from matching the value
// to a configuration variable + handling if the value doesn't match any of our defaults
//

template <typename T, typename A>
concept UnreferencedArrayResult = std::same_as<std::remove_cvref_t<T>, A>;

template< typename T >
concept IndexibleLabels = requires (T & t, const std::size_t i) {
    { t[i] } -> UnreferencedArrayResult< const char* >;
};
template< typename T, typename V >
concept IndexibleTypes = requires (T & t, const std::size_t i) {
    { t[i] } -> UnreferencedArrayResult< V >;
};

template< typename _entryT, IndexibleLabels _containerL, IndexibleTypes<_entryT> _containerT >
requires std::equality_comparable<_entryT>
std::string ValueArrayPreviewString(
    const _containerL&  entryLabels,
    const _containerT&  entryValues,
    _entryT&            variable )
{
    ABSL_ASSERT( entryLabels.size() == entryValues.size() );

    for ( std::size_t optI = 0; optI < entryLabels.size(); optI++ )
    {
        if ( entryValues[optI] == variable )
            return entryLabels[optI];
    }
    return fmt::format( "{} (Custom)", variable );
}

template< typename _entryT, IndexibleLabels _containerL, IndexibleTypes<_entryT> _containerT >
requires std::equality_comparable<_entryT>
bool ValueArrayComboBox(
    const char*         title,
    const char*         label,
    const _containerL&  entryLabels,
    const _containerT&  entryValues,
    _entryT&            variable,
    std::string&        previewString,
    const bool          addYOffset )
{
    ABSL_ASSERT( entryLabels.size() == entryValues.size() );

    ImGui::TextUnformatted( title );
    ImGui::SameLine();

    if ( addYOffset )
        ImGui::SetCursorPosY( ImGui::GetCursorPosY() - 3.0f );

    bool changed = false;
    if ( ImGui::BeginCombo( label, previewString.c_str() ) )
    {
        for ( std::size_t optI = 0; optI < entryLabels.size(); optI++ )
        {
            const bool selected = ( entryValues[optI] == variable );
            if ( ImGui::Selectable( entryLabels[optI], selected ) )
            {
                variable        = entryValues[optI];
                previewString   = ValueArrayPreviewString( entryLabels, entryValues, variable );
                changed         = true;
            }
            if ( selected )
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    return changed;
}

} // namespace ImGui
