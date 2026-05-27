#pragma once

#include "Core/EditModelToolFilterPolicy.h"

class AActor;

/** Editor-session toolbar state shared across EditModelTool module translation units. */
namespace EditModelTool
{
	FEditModelToolFilterSettings& SessionFilterSettings();

	/** Modeling same-normal: half-angle in degrees (UI + runtime; clamped 0.01–90 when used). */
	float& SessionSameNormalAngleToleranceDegrees();

	bool ActorMatchesGlobalSessionFilter(AActor* Actor);
}
