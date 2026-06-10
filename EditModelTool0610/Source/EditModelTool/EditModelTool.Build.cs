using UnrealBuildTool;

public class EditModelTool : ModuleRules
{
    public EditModelTool(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // Single module PCH: avoids MSVC LNK2011 when Live Coding patches multiple TUs mixed with different shared-PCH fingerprints.
        PrivatePCHHeaderFile = "Private/EditModelToolPrivatePCH.h";

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                // Editor plugin: stabilize editor API link (viewport defs, mode tools)
                "UnrealEd",
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "ApplicationCore",
                "Slate",
                "SlateCore",
                "InputCore",
                "LevelEditor",
                "ToolMenus",
                "EditorSubsystem",
                "EditorFramework",
                "DatasmithContent",
                "MeshDescription",
                "StaticMeshDescription",
                "ModelingToolsEditorMode",
                "ModelingComponents",
                "DynamicMesh",
                "GeometryFramework",
                "InteractiveToolsFramework",
                "EditorInteractiveToolsFramework",
                // Resolves Audio::NAME_* refs from headers + helps Live Coding patch link
                "AudioMixerCore"
            });
    }
}
