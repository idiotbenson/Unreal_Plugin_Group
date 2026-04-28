using UnrealBuildTool;

public class EditModelTool : ModuleRules
{
    public EditModelTool(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.NoPCHs;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Slate",
                "SlateCore",
                "InputCore",
                "UnrealEd",
                "LevelEditor",
                "ToolMenus",
                "EditorSubsystem",
                "EditorFramework",
                "DatasmithContent"
            });
    }
}
