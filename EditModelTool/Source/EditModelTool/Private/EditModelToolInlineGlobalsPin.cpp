#include "CoreMinimal.h"
#include "AudioMixerTypes.h"
#include "Viewports.h"

// Live Coding patch links only a subset of objects and can miss engine inline globals that MSVC
// emits as external. Force ODR-use in this TU so names live in EditModelTool.dll.
namespace EditModelTool_InlineGlobalsPin
{
	namespace
	{
		static const FVector* const GViewportDefaultLocationPtr = &EditorViewportDefs::DefaultPerspectiveViewLocation;
		static const FRotator* const GViewportDefaultRotationPtr = &EditorViewportDefs::DefaultPerspectiveViewRotation;
		static const float* const GViewportDefaultFovPtr = &EditorViewportDefs::DefaultPerspectiveFOVAngle;

		static const FName* const GAudioPlatformSpecific = &Audio::NAME_PLATFORM_SPECIFIC;
		static const FName* const GAudioProjectDefined = &Audio::NAME_PROJECT_DEFINED;
		static const FName* const GAudioBinka = &Audio::NAME_BINKA;
		static const FName* const GAudioAdpcm = &Audio::NAME_ADPCM;
		static const FName* const GAudioPcm = &Audio::NAME_PCM;
		static const FName* const GAudioOpus = &Audio::NAME_OPUS;
		static const FName* const GAudioRada = &Audio::NAME_RADA;
		static const FName* const GAudioOgg = &Audio::NAME_OGG;

		struct FPinRegistrar
		{
			FPinRegistrar()
			{
				volatile float V = (*GViewportDefaultLocationPtr).X + (*GViewportDefaultRotationPtr).Yaw + *GViewportDefaultFovPtr;
				volatile int32 L = 0;
				L += GAudioPlatformSpecific->ToString().Len();
				L += GAudioProjectDefined->ToString().Len();
				L += GAudioBinka->ToString().Len();
				L += GAudioAdpcm->ToString().Len();
				L += GAudioPcm->ToString().Len();
				L += GAudioOpus->ToString().Len();
				L += GAudioRada->ToString().Len();
				L += GAudioOgg->ToString().Len();
				(void)V;
				(void)L;
			}
		};

		static FPinRegistrar GPinRegistrar;
	}
}
