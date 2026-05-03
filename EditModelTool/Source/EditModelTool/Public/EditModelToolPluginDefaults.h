#pragma once

#include "CoreMinimal.h"

/**
 * Compile-time defaults for Edit Model Tool (recompile editor after changes).
 * The batch dialog can override same-normal angle at runtime via session state.
 */
namespace EditModelTool::PluginDefaults
{
	/**
	 * Half-angle (degrees) for same-normal expansion in Modeling tools.
	 * Faces whose normals fall within this cone of the averaged seed normal are selected.
	 * Clamped at runtime to [0.01, 90].
	 */
	inline constexpr float SameNormalFaceAngleToleranceDegrees = 8.f;
}
