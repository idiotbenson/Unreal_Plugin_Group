#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

namespace EditModelToolStaticMeshGeometry
{
	/** Set when fingerprint hashes sorted LOD0 vertices + canonical triangles/edges (MeshDescription or render buffers). */
	constexpr uint64 StrongLod0FingerprintBit = (1ull << 62);

	bool IsStrongLod0Fingerprint(uint64 Fingerprint);
	bool Lod0FingerprintsMatch(uint64 FingerprintA, uint64 FingerprintB);

	/** LOD0 geometry fingerprint: strong path uses quantized vertex/triangle topology; weak path uses tri/vert counts + bounds. */
	bool TryComputeLod0Fingerprint(const UStaticMesh* Mesh, uint64& OutFingerprint);
}
