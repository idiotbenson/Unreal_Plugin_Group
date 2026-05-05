#include "EditModelToolPrivatePCH.h"

#include "EditModelToolSession.h"
#include "EditModelToolPluginDefaults.h"

namespace EditModelTool
{
	static FEditModelToolFilterSettings GSessionFilter;
	static float GSameNormalAngleToleranceDegrees =
		EditModelTool::PluginDefaults::SameNormalFaceAngleToleranceDegrees;

	FEditModelToolFilterSettings& SessionFilterSettings()
	{
		return GSessionFilter;
	}

	float& SessionSameNormalAngleToleranceDegrees()
	{
		return GSameNormalAngleToleranceDegrees;
	}

	bool ActorMatchesGlobalSessionFilter(AActor* Actor)
	{
		return EditModelToolFilterPolicy::ActorMatchesGlobalRule(Actor, GSessionFilter);
	}
}
