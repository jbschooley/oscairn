/*
 * Modular engine: the patch.
 *
 * A patch is a ValueTree, so the program can keep it in the project (saved with
 * it, undoable):
 *
 *   <MODULAR_PATCH name="Init" voices="8">
 *     <MODULE id="1" type="osc" col="0" row="0" wave="2" level="0.5"/>
 *     <MODULE id="3" type="vca" col="0" row="1" gain="0">
 *       <MOD param="gain" source="module:2" depth="1"/>
 *     </MODULE>
 *     <MODULE id="2" type="adsr" col="1" row="0" attack="0.005"/>
 *   </MODULAR_PATCH>
 *
 * Module properties named after a parameter hold its value in that parameter's
 * units; missing ones take the default. Audio runs down each column in row order
 * and the columns are summed. A MOD adds `depth` (in normalised knob travel,
 * -1..1) times a source to one of the module's parameters; the source is a
 * built-in ("velocity", "macro1"...) or a modulator module ("module:<id>").
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

namespace mt::modular
{
    namespace PatchIDs
    {
        inline const juce::Identifier MODULAR_PATCH{ "MODULAR_PATCH" };
        inline const juce::Identifier MODULE{ "MODULE" };
        inline const juce::Identifier MOD{ "MOD" };
        inline const juce::Identifier name{ "name" };
        inline const juce::Identifier voices{ "voices" };
        inline const juce::Identifier id{ "id" };
        inline const juce::Identifier type{ "type" };
        inline const juce::Identifier col{ "col" };
        inline const juce::Identifier row{ "row" };
        inline const juce::Identifier param{ "param" };
        inline const juce::Identifier source{ "source" };
        inline const juce::Identifier depth{ "depth" };
    } // namespace PatchIDs

    constexpr int maxVoices     = 16;
    constexpr int defaultVoices = 8;
    constexpr int gridColumns   = 8; // what the editor shows; the engine does not care
    constexpr int gridRows      = 6;

    /// A patch read out of its tree: plain data the engine can build from.
    struct PatchDesc
    {
        struct Mod
        {
            juce::String param, source;
            float        depth{ 0 };
        };

        struct Module
        {
            int                id{ 0 };
            const ModuleSpec*  spec{ nullptr };
            int                col{ 0 }, row{ 0 };
            std::vector<float> values; // one per spec parameter, in its units
            std::vector<Mod>   mods;
        };

        juce::String        name;
        int                 voices{ defaultVoices };
        std::vector<Module> modules; // column, then row order; unknown types are skipped

        static PatchDesc fromTree( const juce::ValueTree& Patch );

        /// Everything except knob values and depths. Two patches with the same key
        /// differ only in values, which the engine can change without rebuilding.
        juce::String structureKey() const;

        /// Index into `modules` of the module with this id, or -1.
        int indexOf( int Id ) const;
    };

    /// Builds patch trees in code (factory presets, tests).
    class PatchBuilder
    {
    public:
        explicit PatchBuilder( const juce::String& Name, int Voices = defaultVoices );

        /// Adds a module and returns its id. Values not given take their defaults.
        int add( const juce::String& Type, int Col, int Row, std::initializer_list<std::pair<const char*, float>> Values = {} );
        PatchBuilder& mod( int Target, const juce::String& Param, const juce::String& Source, float Depth );

        static juce::String module( int Id ) { return "module:" + juce::String( Id ); }

        juce::ValueTree tree;

    private:
        int nextId{ 1 };
    };

    juce::StringArray getPresetNames();
    juce::ValueTree   createPreset( int Index ); // index 0 is the default patch

    /// Edits a stored patch the way the editor screen does: every call is a tree
    /// change through the undo manager (the caller begins transactions), and the
    /// program forwards it to the engine. The first edit of a factory preset marks
    /// its name ("FM Bell *") so it no longer reads as the untouched preset.
    class PatchEditor
    {
    public:
        PatchEditor( juce::ValueTree Patch, juce::UndoManager* UM ) : patch( std::move( Patch ) ), um( UM ) {}

        bool isValid() const { return patch.isValid(); }

        juce::ValueTree module( int Id ) const;
        juce::ValueTree moduleAt( int Col, int Row ) const;
        static const ModuleSpec* specOf( const juce::ValueTree& Module );

        /// Returns the new module's id, or 0 if the cell is taken or the type unknown.
        int  addModule( const juce::String& Type, int Col, int Row );
        /// Also removes the modulations it fed.
        void removeModule( int Id );
        /// Moves a module; one already in the target cell takes its old place.
        void moveModule( int Id, int Col, int Row );

        float getValue( int Id, const juce::String& Param ) const;
        void  setValue( int Id, const juce::String& Param, float Value );

        juce::ValueTree addMod( int Target, const juce::String& Param, const juce::String& Source, float Depth );
        void            setModDepth( juce::ValueTree Mod, float Depth );
        void            removeMod( juce::ValueTree Mod );

        int  getVoices() const;
        void setVoices( int Voices );

        /// Modulation sources this patch offers: the built-ins, then its modulator
        /// modules, as { stored id, display name }.
        std::vector<std::pair<juce::String, juce::String>> getSources() const;
        /// "LFO 2": type name, numbered in grid order among modules of that type.
        juce::String moduleLabel( int Id ) const;
        juce::String sourceLabel( const juce::String& Source ) const;

    private:
        void markEdited();
        int  nextId() const;

        juce::ValueTree    patch;
        juce::UndoManager* um;
    };
} // namespace mt::modular
