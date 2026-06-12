#include "Core/EditModelToolStaticMeshGeometryFingerprint.h"

#include "EditModelToolPrivatePCH.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshResources.h"

namespace
{
	struct FQuantizedTriple
	{
		int64 X = 0;
		int64 Y = 0;
		int64 Z = 0;

		bool operator<(const FQuantizedTriple& O) const
		{
			if (X != O.X)
			{
				return X < O.X;
			}
			if (Y != O.Y)
			{
				return Y < O.Y;
			}
			return Z < O.Z;
		}
	};

	struct FCanonEdge
	{
		FQuantizedTriple Lo;
		FQuantizedTriple Hi;

		bool operator<(const FCanonEdge& O) const
		{
			if (Lo < O.Lo)
			{
				return true;
			}
			if (O.Lo < Lo)
			{
				return false;
			}
			return Hi < O.Hi;
		}
	};

	struct FCanonTriangle
	{
		FQuantizedTriple A;
		FQuantizedTriple B;
		FQuantizedTriple C;

		bool operator<(const FCanonTriangle& O) const
		{
			if (A < O.A)
			{
				return true;
			}
			if (O.A < A)
			{
				return false;
			}
			if (B < O.B)
			{
				return true;
			}
			if (O.B < B)
			{
				return false;
			}
			return C < O.C;
		}
	};

	FQuantizedTriple QuantizePosition(const FVector3f& P3f, const double Scale)
	{
		FQuantizedTriple Q;
		Q.X = static_cast<int64>(FMath::RoundToInt(static_cast<double>(P3f.X) * Scale));
		Q.Y = static_cast<int64>(FMath::RoundToInt(static_cast<double>(P3f.Y) * Scale));
		Q.Z = static_cast<int64>(FMath::RoundToInt(static_cast<double>(P3f.Z) * Scale));
		return Q;
	}

	FCanonEdge MakeCanonEdge(const FQuantizedTriple& U, const FQuantizedTriple& V)
	{
		FCanonEdge E;
		if (U < V)
		{
			E.Lo = U;
			E.Hi = V;
		}
		else if (V < U)
		{
			E.Lo = V;
			E.Hi = U;
		}
		else
		{
			E.Lo = U;
			E.Hi = V;
		}
		return E;
	}

	uint64 Fnv1a64_Update(uint64 Hash, uint64 Word)
	{
		constexpr uint64 FnvPrime = 1099511628211ull;
		Hash ^= Word;
		return Hash * FnvPrime;
	}

	uint64 HashSortedTriples(const TArray<FQuantizedTriple>& Sorted)
	{
		uint64 H = 1469598103934665603ull;
		for (const FQuantizedTriple& T : Sorted)
		{
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.X)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.Y)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.Z)));
		}
		return H;
	}

	uint64 HashSortedCanonTriangles(const TArray<FCanonTriangle>& Sorted)
	{
		uint64 H = 975531335987437023ull;
		for (const FCanonTriangle& T : Sorted)
		{
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.A.X)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.A.Y)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.A.Z)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.B.X)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.B.Y)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.B.Z)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.C.X)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.C.Y)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(T.C.Z)));
		}
		return H;
	}

	uint64 HashSortedCanonEdges(const TArray<FCanonEdge>& Sorted)
	{
		uint64 H = 1229492661964435241ull;
		for (const FCanonEdge& E : Sorted)
		{
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Lo.X)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Lo.Y)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Lo.Z)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Hi.X)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Hi.Y)));
			H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<int64>(E.Hi.Z)));
		}
		return H;
	}

	bool TryFinalizeStrongFingerprint(
		const TArray<FQuantizedTriple>& VertexMultiset,
		const TArray<FCanonTriangle>& CanonTriangles,
		const TArray<FCanonEdge>& CanonEdges,
		const int32 TriCount,
		const int32 VertCount,
		uint64& OutFingerprint)
	{
		if (TriCount <= 0 || VertCount <= 0 || VertexMultiset.IsEmpty() || CanonTriangles.IsEmpty())
		{
			return false;
		}

		uint64 H = HashSortedTriples(VertexMultiset);
		H = Fnv1a64_Update(H, HashSortedCanonTriangles(CanonTriangles));
		H = Fnv1a64_Update(H, HashSortedCanonEdges(CanonEdges));
		H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(TriCount)));
		H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(VertCount)));
		H |= EditModelToolStaticMeshGeometry::StrongLod0FingerprintBit;
		OutFingerprint = H;
		return true;
	}

	bool TryComputeStrongFingerprintFromMeshDescription(const UStaticMesh* Mesh, uint64& OutFingerprint)
	{
		const FMeshDescription* Desc = Mesh->GetMeshDescription(0);
		if (!Desc || Desc->Triangles().Num() <= 0 || Desc->Vertices().Num() <= 0)
		{
			return false;
		}

		const TVertexAttributesConstRef<FVector3f> VertexPositions = Desc->GetVertexPositions();
		TArray<FQuantizedTriple> VertexMultiset;
		VertexMultiset.Reserve(Desc->Vertices().Num());
		constexpr double Scale = 10000.0;
		for (const FVertexID VertexID : Desc->Vertices().GetElementIDs())
		{
			VertexMultiset.Add(QuantizePosition(VertexPositions[VertexID], Scale));
		}
		VertexMultiset.Sort();

		TArray<FCanonTriangle> CanonTriangles;
		TArray<FCanonEdge> CanonEdges;
		CanonTriangles.Reserve(Desc->Triangles().Num());
		CanonEdges.Reserve(Desc->Triangles().Num() * 3);

		for (const FTriangleID TriID : Desc->Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexInstanceID> Vis = Desc->GetTriangleVertexInstances(TriID);
			if (Vis.Num() < 3)
			{
				return false;
			}
			TArray<FQuantizedTriple> Corners;
			Corners.Reserve(3);
			for (int32 i = 0; i < 3; ++i)
			{
				const FVertexID Vid = Desc->GetVertexInstanceVertex(Vis[i]);
				Corners.Add(QuantizePosition(VertexPositions[Vid], Scale));
			}
			Corners.Sort();
			FCanonTriangle CT;
			CT.A = Corners[0];
			CT.B = Corners[1];
			CT.C = Corners[2];
			CanonTriangles.Add(CT);
			CanonEdges.Add(MakeCanonEdge(Corners[0], Corners[1]));
			CanonEdges.Add(MakeCanonEdge(Corners[1], Corners[2]));
			CanonEdges.Add(MakeCanonEdge(Corners[0], Corners[2]));
		}
		CanonTriangles.Sort();
		CanonEdges.Sort();

		return TryFinalizeStrongFingerprint(
			VertexMultiset,
			CanonTriangles,
			CanonEdges,
			Desc->Triangles().Num(),
			Desc->Vertices().Num(),
			OutFingerprint);
	}

	bool TryComputeStrongFingerprintFromRenderData(const UStaticMesh* Mesh, uint64& OutFingerprint)
	{
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
		{
			return false;
		}

		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		const FPositionVertexBuffer& PosBuf = LOD.VertexBuffers.PositionVertexBuffer;
		const int32 VertCount = PosBuf.GetNumVertices();
		const int32 TriCount = LOD.GetNumTriangles();
		if (VertCount <= 0 || TriCount <= 0)
		{
			return false;
		}

		TArray<FQuantizedTriple> VertexMultiset;
		VertexMultiset.Reserve(VertCount);
		constexpr double Scale = 10000.0;
		for (int32 VertIdx = 0; VertIdx < VertCount; ++VertIdx)
		{
			VertexMultiset.Add(QuantizePosition(PosBuf.VertexPosition(VertIdx), Scale));
		}
		VertexMultiset.Sort();

		TArray<FCanonTriangle> CanonTriangles;
		TArray<FCanonEdge> CanonEdges;
		CanonTriangles.Reserve(TriCount);
		CanonEdges.Reserve(TriCount * 3);

		for (int32 TriIdx = 0; TriIdx < TriCount; ++TriIdx)
		{
			TArray<FQuantizedTriple> Corners;
			Corners.Reserve(3);
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 VertIdx = LOD.IndexBuffer.GetIndex(TriIdx * 3 + Corner);
				if (VertIdx >= static_cast<uint32>(VertCount))
				{
					return false;
				}
				Corners.Add(QuantizePosition(PosBuf.VertexPosition(static_cast<int32>(VertIdx)), Scale));
			}
			Corners.Sort();
			FCanonTriangle CT;
			CT.A = Corners[0];
			CT.B = Corners[1];
			CT.C = Corners[2];
			CanonTriangles.Add(CT);
			CanonEdges.Add(MakeCanonEdge(Corners[0], Corners[1]));
			CanonEdges.Add(MakeCanonEdge(Corners[1], Corners[2]));
			CanonEdges.Add(MakeCanonEdge(Corners[0], Corners[2]));
		}
		CanonTriangles.Sort();
		CanonEdges.Sort();

		return TryFinalizeStrongFingerprint(
			VertexMultiset,
			CanonTriangles,
			CanonEdges,
			TriCount,
			VertCount,
			OutFingerprint);
	}

	bool TryComputeWeakFingerprint(const UStaticMesh* Mesh, uint64& OutFingerprint)
	{
		const int32 TriRender = Mesh->GetNumTriangles(0);
		const int32 VertRender = Mesh->GetNumVertices(0);
		if (TriRender < 0 || VertRender < 0)
		{
			return false;
		}

		const FBox B = Mesh->GetBoundingBox();
		if (!B.IsValid)
		{
			return false;
		}

		auto Qc = [](const double V) -> int64
		{
			return static_cast<int64>(FMath::RoundToInt(V * 1000.0));
		};

		uint64 H = 2166136261ull;
		H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(TriRender)));
		H = Fnv1a64_Update(H, static_cast<uint64>(static_cast<uint32>(VertRender)));
		H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Min.X)));
		H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Min.Y)));
		H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Min.Z)));
		H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Max.X)));
		H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Max.Y)));
		H = Fnv1a64_Update(H, static_cast<uint64>(Qc(B.Max.Z)));
		H &= ~EditModelToolStaticMeshGeometry::StrongLod0FingerprintBit;
		OutFingerprint = H;
		return true;
	}
}

bool EditModelToolStaticMeshGeometry::IsStrongLod0Fingerprint(const uint64 Fingerprint)
{
	return Fingerprint != 0 && (Fingerprint & StrongLod0FingerprintBit) != 0;
}

bool EditModelToolStaticMeshGeometry::Lod0FingerprintsMatch(const uint64 FingerprintA, const uint64 FingerprintB)
{
	return FingerprintA != 0 && FingerprintA == FingerprintB;
}

bool EditModelToolStaticMeshGeometry::TryComputeLod0Fingerprint(const UStaticMesh* Mesh, uint64& OutFingerprint)
{
	OutFingerprint = 0;
	if (!Mesh)
	{
		return false;
	}

	if (TryComputeStrongFingerprintFromMeshDescription(Mesh, OutFingerprint)
		|| TryComputeStrongFingerprintFromRenderData(Mesh, OutFingerprint))
	{
		return true;
	}

	return TryComputeWeakFingerprint(Mesh, OutFingerprint);
}
