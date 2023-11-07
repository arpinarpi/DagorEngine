#include "genVisibility.h"
#include <rendInst/visibility.h>
#include <rendInst/gpuObjects.h>

#include "render/genRender.h"
#include "riGen/genObjUtil.h"
#include "visibility/cullingMath.h"

#include <osApiWrappers/dag_cpuJobs.h>


RiGenVisibility *rendinst::createRIGenVisibility(IMemAlloc *mem)
{
  RiGenVisibility *visibility = (RiGenVisibility *)mem->alloc(sizeof(RiGenVisibility) * 2);
  new (visibility, _NEW_INPLACE) RiGenVisibility(mem);
  new (visibility + 1, _NEW_INPLACE) RiGenVisibility(mem);
  return visibility;
}

void rendinst::setRIGenVisibilityDistMul(RiGenVisibility *visibility, float dist_mul)
{
  if (visibility)
  {
    visibility[0].setRiDistMul(dist_mul);
    visibility[1].setRiDistMul(dist_mul);
  }
}

void rendinst::destroyRIGenVisibility(RiGenVisibility *visibility)
{
  if (visibility)
  {
    gpuobjects::disable_for_visibility(visibility);
    IMemAlloc *mem = visibility[0].getmem();
    visibility[0].~RiGenVisibility();
    visibility[1].~RiGenVisibility();
    mem->free(visibility);
  }
}

void rendinst::setRIGenVisibilityMinLod(RiGenVisibility *visibility, int ri_lod, int ri_extra_lod)
{
  visibility[0].forcedLod = visibility[1].forcedLod = ri_lod;
  visibility[0].riex.forcedExtraLod = visibility[1].riex.forcedExtraLod = ri_extra_lod;
}

void rendinst::setRIGenVisibilityAtestSkip(RiGenVisibility *visibility, bool skip_atest, bool skip_noatest)
{
  if (skip_noatest && skip_atest)
  {
    visibility[0].atest_skip_mask = visibility->RENDER_ALL;
    G_ASSERTF(0, "it doesn't make sense to render empty rigen list (there is no atest and no non-atest)");
    return;
  }
  visibility[0].atest_skip_mask =
    skip_atest ? visibility->SKIP_ATEST : (skip_noatest ? visibility->SKIP_NO_ATEST : visibility->RENDER_ALL);
}

void rendinst::setRIGenVisibilityRendering(RiGenVisibility *visibility, VisibilityRenderingFlags v) { visibility[0].rendering = v; }

bool rendinst::prepareRIGenVisibility(const Frustum &frustum, const Point3 &vpos, RiGenVisibility *visibility, bool forShadow,
  Occlusion *use_occlusion, bool for_visual_collision, const rendinst::VisibilityExternalFilter &external_filter)
{
  if (!RendInstGenData::renderResRequired || RendInstGenData::isLoading)
    return false;
  G_ASSERT(visibility);
  bool ret = false;
  if (!external_filter)
  {
    FOR_EACH_RG_LAYER_DO (rgl)
      if (rgl->prepareVisibility(frustum, vpos, visibility[_layer], forShadow, {}, use_occlusion, for_visual_collision))
        ret = true;
  }
  else
  {
    FOR_EACH_RG_LAYER_DO (rgl)
      if (rgl->prepareVisibility<true>(frustum, vpos, visibility[_layer], forShadow, {}, use_occlusion, for_visual_collision,
            external_filter))
        ret = true;
  }
  return ret;
}

void rendinst::sortRIGenVisibility(RiGenVisibility *visibility, const Point3 &viewPos, const Point3 &viewDir, float vertivalFov,
  float horizontalFov, float areaThreshold)
{
  G_ASSERT(visibility);
  FOR_EACH_RG_LAYER_RENDER (rgl, rgRenderMaskO)
    rgl->sortRIGenVisibility(visibility[_layer], viewPos, viewDir, vertivalFov, horizontalFov, areaThreshold);
}

static inline bool getSubCellsVisibility(vec3f curViewPos, const Frustum &frustum, const bbox3f *__restrict bbox,
  uint16_t &ranges_count, uint8_t *ranges, float &cellDist, Occlusion *use_occlusion)
{
  G_STATIC_ASSERT(RendInstGenData::SUBCELL_DIV == 8);
  G_STATIC_ASSERT(RendInstGenData::SUBCELL_DIV * RendInstGenData::SUBCELL_DIV <= 256);

  int nPlanes;
  vec4f planes[6];
  int cellIntersection = test_box_planes(bbox[0].bmin, bbox[0].bmax, frustum, planes, nPlanes);
  if (cellIntersection == Frustum::OUTSIDE)
  {
    ranges_count = 0;
    return false;
  }
  if (use_occlusion)
  {
    if (use_occlusion->isOccludedBox(bbox[0].bmin, bbox[0].bmax))
    {
      ranges_count = 0;
      return false;
    }
  }
  cellDist = v_extract_x(v_sqrt_x(v_distance_sq_to_bbox_x(bbox[0].bmin, bbox[0].bmax, curViewPos)));
  if (cellIntersection == Frustum::INSIDE) //-V1051
  {
    if (use_occlusion)
    {
      int idx = 1;
      for (; idx < RendInstGenData::SUBCELL_DIV * RendInstGenData::SUBCELL_DIV + 1; ++idx)
        if (!use_occlusion->isOccludedBox(bbox[idx].bmin, bbox[idx].bmax))
          break;

      int idx2 = RendInstGenData::SUBCELL_DIV * RendInstGenData::SUBCELL_DIV;
      for (; idx2 > idx; --idx2)
        if (!use_occlusion->isOccludedBox(bbox[idx2].bmin, bbox[idx2].bmax))
          break;
      if (idx2 < idx)
      {
        ranges_count = 0;
        return false;
      }
      ranges[0] = idx - 1;
      ranges[1] = idx2 - 1;
    }
    else
    {
      ranges[0] = 0;
      ranges[1] = RendInstGenData::SUBCELL_DIV * RendInstGenData::SUBCELL_DIV - 1;
    }
    ranges_count = 1;
    return true;
  }


  ranges_count = 0;
  for (int idx = 1; idx < RendInstGenData::SUBCELL_DIV * RendInstGenData::SUBCELL_DIV + 1; ++idx)
  {
    if (test_box_planesb(bbox[idx].bmin, bbox[idx].bmax, planes, nPlanes))
    {
      if (use_occlusion)
      {
        if (use_occlusion->isOccludedBox(bbox[idx].bmin, bbox[idx].bmax))
          continue;
      }
      if (!ranges_count || ranges[-1] != idx - 2)
      {
        ranges[0] = ranges[1] = idx - 1;
        ranges += 2;
        ranges_count++;
      }
      else
        ranges[-1] = idx - 1;
    }
  }
  return ranges_count != 0;
}

struct RendInstGenRenderPrepareData
{
  static constexpr int MAX_VISIBLE_CELLS = 256;
  int totalVisibleCells;
  struct Cell
  {
    float distance;
    uint16_t x, z;
    uint16_t rangesStart;
    uint16_t rangesCount; // it is actually only 5 bit, but for the sake of alignment, keep it 16bit
  };
  carray<Cell, MAX_VISIBLE_CELLS> cells = {};
  carray<uint8_t, MAX_VISIBLE_CELLS * 2 * (RendInstGenData::SUBCELL_DIV * RendInstGenData::SUBCELL_DIV / 2 + 1)> cellRanges = {};
  int visibleCellsCount() const { return totalVisibleCells; }
  RendInstGenRenderPrepareData() : totalVisibleCells(0) {}
};

struct SortByY
{
  bool operator()(const IPoint2 &a, const IPoint2 &b) const { return a.y < b.y; }
};

template <bool use_external_filter>
bool RendInstGenData::prepareVisibility(const Frustum &frustum, const Point3 &camera_pos, RiGenVisibility &visibility, bool forShadow,
  rendinst::LayerFlags layer_flags, Occlusion *use_occlusion, bool for_visual_collision,
  const rendinst::VisibilityExternalFilter &external_filter)
{
  TIME_D3D_PROFILE(prepare_ri_visibility);
  visibility.vismask = 0;
  if (for_visual_collision && rendinst::isRgLayerPrimary(rtData->layerIdx))
    return false;

  if (rendinst::ri_game_render_mode == 0)
    use_occlusion = nullptr;
  vec3f curViewPos = v_ldu(&camera_pos.x);
  Tab<RenderRanges> &riRenderRanges = visibility.renderRanges;
  Frustum curFrustum = frustum;
  if (!forShadow)
  {
    float maxRIDist = rtData->get_trees_last_range(rtData->rendinstFarPlane);
    shrink_frustum_zfar(curFrustum, curViewPos, v_splats(maxRIDist));
  }

  float forcedLodDist = 0.f;
  float forcedLodDistSq = 0.f;
  Point3_vec4 viewPos = camera_pos;
  if (visibility.forcedLod >= 0)
  {
    bbox3f box;
    frustum.calcFrustumBBox(box);
    curViewPos = v_bbox3_center(box);
    v_st(&viewPos.x, curViewPos);

    float rad = v_extract_x(v_bbox3_outer_rad(box));
    forcedLodDist = rad;
    forcedLodDistSq = forcedLodDist * forcedLodDist;
  }
  bbox3f worldBBox;
  float grid2worldcellSz = grid2world * cellSz;

  curFrustum.calcFrustumBBox(worldBBox);
  if (!check_occluders)
    use_occlusion = nullptr;

  vec4f worldBboxXZ = v_perm_xzac(worldBBox.bmin, worldBBox.bmax);
  vec4f grid2worldcellSzV = v_splats(grid2worldcellSz);
  worldBboxXZ = v_add(worldBboxXZ, v_perm_xzac(v_neg(grid2worldcellSzV), grid2worldcellSzV));
  vec4f regionV = v_sub(worldBboxXZ, world0Vxz);
  regionV = v_max(v_mul(regionV, invGridCellSzV), v_zero());
  regionV = v_min(regionV, lastCellXZXZ);
  vec4i regionI = v_cvt_floori(regionV);
  DECL_ALIGN16(int, regions[4]);
  v_sti(regions, regionI);
  float grid2worldcellSzDiag = grid2worldcellSz * 1.4142f;
  visibility.subCells.clear();
  visibility.resizeRanges(rtData->riRes.size(), forShadow ? 4 : 8);

  ScopedLockRead lock(rtData->riRwCs);
  RendInstGenRenderPrepareData visData;
  uint8_t *ranges = visData.cellRanges.data();

#define PREPARE_CELL(XX, ZZ)                                                                                                 \
  {                                                                                                                          \
    const RendInstGenData::Cell &cell = cells[cellI];                                                                        \
    const RendInstGenData::CellRtData *crt_ptr = cell.isReady();                                                             \
    if (crt_ptr && visData.totalVisibleCells < visData.MAX_VISIBLE_CELLS)                                                    \
    {                                                                                                                        \
      int cellNo = visData.visibleCellsCount();                                                                              \
      RendInstGenRenderPrepareData::Cell &visCell = visData.cells[cellNo];                                                   \
      if (getSubCellsVisibility(curViewPos, curFrustum, crt_ptr->bbox.data(), visCell.rangesCount, ranges, visCell.distance, \
            use_occlusion))                                                                                                  \
      {                                                                                                                      \
        visCell.x = XX, visCell.z = ZZ;                                                                                      \
        visCell.rangesStart = ranges - visData.cellRanges.data();                                                            \
        visCell.rangesCount *= 2;                                                                                            \
        ranges += visCell.rangesCount;                                                                                       \
        G_ASSERT((int)(ranges - visData.cellRanges.data()) < visData.cellRanges.size());                                     \
        visData.totalVisibleCells++;                                                                                         \
      }                                                                                                                      \
    }                                                                                                                        \
  }

  rtData->loadedCellsBBox.clip(regions[0], regions[1], regions[2], regions[3]);

  float grid2worldCellSz = v_extract_x(invGridCellSzV);
  int startCellX = (int)((viewPos.x - world0x()) * grid2worldCellSz);
  int startCellZ = (int)((viewPos.z - world0z()) * grid2worldCellSz);

  // in not yet known circumstances, values go out of range (in release mode)
  // trying to avoid infinite loop
  if (startCellX == 0x80000000 || startCellZ == 0x80000000)
  {
    debug("prepareVisibility_overflow");
    return false;
  }

  unsigned int startCellI = startCellX + startCellZ * cellNumW;
  {
    int maxRadius =
      max(max((regions[3] - startCellZ), (startCellZ - regions[1])), max((regions[2] - startCellX), (startCellX - regions[0])));
    if (startCellX >= 0 && startCellX < cellNumW && startCellZ < cellNumH && startCellZ >= 0)
    {
      int cellI = startCellI;
      PREPARE_CELL(startCellX, startCellZ);
    }
    int minX, maxX;
    int minZ, maxZ;
    for (int radius = 1; radius <= maxRadius; ++radius)
    {
      minX = startCellX - radius, maxX = startCellX + radius;
      minZ = startCellZ - radius, maxZ = startCellZ + radius;

      if (minZ >= regions[1] && minZ <= regions[3])
        for (int x = max(minX, regions[0]), cellI = minZ * cellNumW + x; x <= min(maxX, regions[2]); x++, cellI++)
          PREPARE_CELL(x, minZ);

      if (maxZ <= regions[3] && maxZ >= regions[1])
        for (int x = max(minX, regions[0]), cellI = maxZ * cellNumW + x; x <= min(maxX, regions[2]); x++, cellI++)
          PREPARE_CELL(x, maxZ);

      if (minX >= regions[0] && minX <= regions[2])
        for (int z = max(minZ + 1, regions[1]), cellI = z * cellNumW + minX; z <= min(maxZ - 1, regions[3]); z++, cellI += cellNumW)
          PREPARE_CELL(minX, z);

      if (maxX <= regions[2] && maxX >= regions[0])
        for (int z = max(minZ + 1, regions[1]), cellI = z * cellNumW + maxX; z <= min(maxZ - 1, regions[3]); z++, cellI += cellNumW)
          PREPARE_CELL(maxX, z);
    }
  }
#undef PREPARE_CELL
  if (!visData.visibleCellsCount())
    return false;
  mem_set_0(visibility.instNumberCounter);
  float subCellOfsSize = grid2worldcellSz * ((rendinst::render::per_instance_visibility_for_everyone ? 0.75f : 0.25f) *
                                              (rendinst::render::globalDistMul * 1.f / RendInstGenData::SUBCELL_DIV));

  static constexpr int MAX_PER_INSTANCE_CELLS = 7;
  vec3f v_cell_add[MAX_PER_INSTANCE_CELLS], v_cell_mul[MAX_PER_INSTANCE_CELLS];
  static constexpr int MAX_INSTANCES_TO_SORT = 256;
  carray<carray<IPoint2, MAX_INSTANCES_TO_SORT>, rendinst::MAX_LOD_COUNT> perInstanceDistance;
  carray<carray<vec4f, MAX_INSTANCES_TO_SORT>, rendinst::MAX_LOD_COUNT> sortedInstances;
  if (rendinst::render::per_instance_visibility)
  {
    visibility.startTreeInstances();
    for (int vi = 0; vi < min<int>(MAX_PER_INSTANCE_CELLS, visData.visibleCellsCount()); ++vi)
    {
      int x = visData.cells[vi].x;
      int z = visData.cells[vi].z;
      int cellId = x + z * cellNumW;
      RendInstGenData::Cell &cell = cells[cellId];
      RendInstGenData::CellRtData &crt = *cell.rtData;
      v_cell_add[vi] = crt.cellOrigin;
      v_cell_mul[vi] = v_mul(rendinst::gen::VC_1div32767, v_make_vec4f(grid2worldcellSz, crt.cellHeight, grid2worldcellSz, 0));
    }
  }

  vec3f v_tree_min, v_tree_max;
  v_tree_min = v_tree_max = v_zero();
  carray<int, RiGenVisibility::PER_INSTANCE_LODS> prevInstancesCount;
  mem_set_0(prevInstancesCount);
  visibility.max_per_instance_instances = 0;

  for (unsigned int ri_idx = 0; ri_idx < rtData->riRes.size(); ri_idx++)
  {
    if (!rtData->rtPoolData[ri_idx] || rendinst::isResHidden(rtData->riResHideMask[ri_idx]))
      continue;

    rendinst::render::RtPoolData &pool = *rtData->rtPoolData[ri_idx];

    if (for_visual_collision &&
        (pool.hasImpostor() || rtData->riPosInst[ri_idx] || rtData->riResElemMask[ri_idx * rendinst::MAX_LOD_COUNT].atest))
      continue;

    bool crossDissolveForPool = false, shortAlpha = forShadow;
    if (pool.hasImpostor() && rendinst::render::per_instance_visibility) // fixme: allow for all types of impostors
    {
      v_tree_min = rtData->riResBb[ri_idx].bmin;
      v_tree_max = rtData->riResBb[ri_idx].bmax;
      crossDissolveForPool = !pool.hasTransitionLod() && rendinst::render::use_cross_dissolve && visibility.forcedLod < 0;
      shortAlpha = false;
    }
    // unsigned int stride = RIGEN_STRIDE_B(rtData->riPosInst[ri_idx], perInstDataDwords);
    int lodCnt = rtData->riResLodCount(ri_idx);
    int farLodNo = lodCnt - 1;
    int alphaFarLodNo = farLodNo + 1;
    int farLodNo_translated = RiGenVisibility::PI_LAST_MESH_LOD;
    G_ASSERT(alphaFarLodNo < rendinst::render::MAX_LOD_COUNT_WITH_ALPHA); // one lod is for transparence
    float farLodStartRange;
    if (farLodNo)
      farLodStartRange =
        pool.hasImpostor() ? rtData->get_trees_range(pool.lodRange[farLodNo - 1]) : rtData->get_range(pool.lodRange[farLodNo - 1]);
    else
      farLodStartRange = 0;
    float farLodEndRange =
      pool.hasImpostor() ? rtData->get_trees_last_range(pool.lodRange[farLodNo]) : rtData->get_last_range(pool.lodRange[farLodNo]);
    farLodEndRange *= visibility.riDistMul;
    farLodStartRange = min(farLodStartRange, farLodEndRange * 0.99f);
    float farLodEndRangeSq = farLodEndRange * farLodEndRange;
    float farLodStartRangeCellDist = max(0.f, farLodStartRange + grid2worldcellSzDiag); // maximum distance for cell caluclation
    float deltaRcp = rtData->transparencyDeltaRcp / farLodEndRange;
    float perInstanceAlphaBlendStartRadius = (rtData->transparencyDeltaRcp - 1.f) / deltaRcp;
    float alphaBlendStartRadius = perInstanceAlphaBlendStartRadius;
    float startAlphaDistance = shortAlpha ? grid2worldcellSz * 0.1 : grid2worldcellSzDiag; // transparent objects can either cast
                                                                                           // shadow or not cast shadow
    if (pool.hasImpostor() && !shortAlpha && rtData->rendinstDistMulFarImpostorTrees > 1.0)
      alphaBlendStartRadius /= rtData->rendinstDistMulFarImpostorTrees;
    // startAlphaDistance*=2;
    float alphaBlendOnCellRadius = alphaBlendStartRadius - startAlphaDistance;
    float alphaBlendOnSubCellRadius = alphaBlendStartRadius - startAlphaDistance * (1.f / RendInstGenData::SUBCELL_DIV);


    float lodDistancesSq[rendinst::render::MAX_LOD_COUNT_WITH_ALPHA] = {0};
    float lodDistancesSq_perInst[RiGenVisibility::PER_INSTANCE_LODS] = {0};
    G_ASSERT(lodCnt < rendinst::render::MAX_LOD_COUNT_WITH_ALPHA);
    int lodTranslation = rendinst::MAX_LOD_COUNT - lodCnt;
    bool hasImpostor = pool.hasImpostor();
    if (!hasImpostor)
    {
      G_ASSERT(lodTranslation > 0);
      if (lodTranslation > 0)
        lodTranslation--;
    }
    for (int lodI = 1; lodI < lodCnt; ++lodI)
    {
      lodDistancesSq[lodI + lodTranslation] =
        pool.hasImpostor() ? rtData->get_trees_range(pool.lodRange[lodI - 1]) : rtData->get_range(pool.lodRange[lodI - 1]);
      lodDistancesSq_perInst[remap_per_instance_lod(lodI + lodTranslation)] = lodDistancesSq[lodI + lodTranslation];
    }
    if (!hasImpostor)
    {
      lodDistancesSq[rendinst::MAX_LOD_COUNT - 1] = 100000;
      lodDistancesSq_perInst[remap_per_instance_lod(rendinst::MAX_LOD_COUNT - 1)] = 100000;
    }

    if (crossDissolveForPool)
    {
      lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD1] =
        lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD0 + 1] - TOTAL_CROSS_DISSOLVE_DIST; // fixed size for cross dissolve
      lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD0] =
        lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD1] + LOD1_DISSOLVE_RANGE; // fixed size for cross dissolve
    }
    else
    {
      lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD1] = lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD0] = 100000;
    }

    for (int lodI = 1 + lodTranslation; lodI < rendinst::MAX_LOD_COUNT; ++lodI)
      lodDistancesSq[lodI] = safediv(lodDistancesSq[lodI], rendinst::render::lodsShiftDistMul);

    for (int lodI = 1 + lodTranslation; lodI < rendinst::MAX_LOD_COUNT; ++lodI)
      lodDistancesSq[lodI] *= lodDistancesSq[lodI];

    lodDistancesSq[lodTranslation] = -1.f; // all rendinst closer, then lod1
    lodDistancesSq_perInst[remap_per_instance_lod(lodTranslation)] = -1.f;
    if (alphaBlendOnSubCellRadius < 0)
    {
      lodDistancesSq[farLodNo_translated + 2] = lodDistancesSq[farLodNo_translated + 1];
      lodDistancesSq_perInst[visibility.PI_ALPHA_LOD] = lodDistancesSq_perInst[visibility.PI_IMPOSTOR_LOD];
    }
    else
    {
      lodDistancesSq[farLodNo_translated + 2] = alphaBlendOnSubCellRadius * alphaBlendOnSubCellRadius;
      lodDistancesSq_perInst[visibility.PI_ALPHA_LOD] = alphaBlendOnSubCellRadius + subCellOfsSize;
    }

    if (crossDissolveForPool)
    {
      lodDistancesSq_perInst[visibility.PI_ALPHA_LOD] = perInstanceAlphaBlendStartRadius;
      // lodDistancesSq_perInst[visibility.PI_ALPHA_LOD-1] = lodDistancesSq_perInst[visibility.PI_ALPHA_LOD];
      lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD0] = min(lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD0],
        lodDistancesSq_perInst[visibility.PI_ALPHA_LOD] - LOD0_DISSOLVE_RANGE);
      lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD1] = min(lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD1],
        lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD0] - LOD1_DISSOLVE_RANGE);
      visibility.crossDissolveRange[ri_idx] = lodDistancesSq_perInst[visibility.PI_DISSOLVE_LOD1];
    }

    for (int lodI = 1 + lodTranslation; lodI < RiGenVisibility::PER_INSTANCE_LODS - 1; ++lodI)
      lodDistancesSq_perInst[lodI] = safediv(lodDistancesSq_perInst[lodI], rendinst::render::lodsShiftDistMul);

    for (int lodI = 1 + lodTranslation; lodI < RiGenVisibility::PER_INSTANCE_LODS; ++lodI)
      lodDistancesSq_perInst[lodI] *= lodDistancesSq_perInst[lodI];

    float distanceToCheckPerInstanceSq =
      hasImpostor ? max(32.f * 32.f, lodDistancesSq_perInst[remap_per_instance_lod(visibility.PI_IMPOSTOR_LOD)])
                  : max(32.f * 32.f, lodDistancesSq_perInst[remap_per_instance_lod(visibility.PI_LAST_MESH_LOD)]);

    visibility.stride = RIGEN_STRIDE_B(pool.hasImpostor(), perInstDataDwords); // rtData->riPosInst[ri_idx]
    visibility.startRenderRange(ri_idx);
    carray<int, RiGenVisibility::PER_INSTANCE_LODS> perInstanceData;
    mem_set_ff(perInstanceData);
    bool pool_front_to_back = rendinst::render::per_instance_front_to_back && (cpujobs::get_core_count() > 3);

    rendinst::gen::RotationPaletteManager::Palette palette =
      rendinst::gen::get_rotation_palette_manager()->getPalette({rtData->layerIdx, (int)ri_idx});
    for (int vi = 0; vi < visData.visibleCellsCount(); vi++)
    {
      int x = visData.cells[vi].x;
      int z = visData.cells[vi].z;
      int cellId = x + z * cellNumW;
      const RendInstGenData::Cell &cell = cells[cellId];
      const RendInstGenData::CellRtData *crt_ptr = cell.isReady();
      if (!crt_ptr)
        continue;
      const RendInstGenData::CellRtData &crt = *crt_ptr;
      if (crt.pools[ri_idx].total - crt.pools[ri_idx].avail < 1)
        continue;

      float minDist = visData.cells[vi].distance;
      float maxDist = visibility.forcedLod < 0 ? farLodEndRange : forcedLodDist;
      if (minDist >= maxDist)
        continue;
      int startVbOfs = crt.getCellSlice(ri_idx, 0).ofs;
      // fixme: we can separate for subcells if alphaBlendOnRadius is splitting cell, so part of subcells will be blended and part not
      carray<int, rendinst::MAX_LOD_COUNT> perInstanceDistanceCnt;
      mem_set_0(perInstanceDistanceCnt);
      if (auto layerForcedLod = rendinst::get_forced_lod(layer_flags);
          (!farLodNo && forShadow) || (minDist > farLodStartRangeCellDist && (minDist > 0.f || minDist <= alphaBlendOnCellRadius)) ||
          layerForcedLod >= 0 || visibility.forcedLod >= 0)
      {
        // add all ranges to far lod

        int lodI;
        if (visibility.forcedLod >= 0)
        {
          lodI = min(visibility.forcedLod, farLodNo);
        }
        else if (layerForcedLod >= 0)
        {
          lodI = clamp(layerForcedLod, 0, rtData->riResLodCount(ri_idx) - 1);
        }
        else
        {
          lodI = (minDist > alphaBlendOnCellRadius && !pool.hasImpostor()) ? alphaFarLodNo : farLodNo;
          if (forShadow && lodI == alphaFarLodNo)
            continue;
        }

        if (use_external_filter)
        {
          if (!external_filter(crt.bbox[0].bmin, crt.bbox[0].bmax))
            continue;
        }

        rtData->riRes[ri_idx]->updateReqLod(min<int>(lodI, rtData->riRes[ri_idx]->lods.size() - 1));
        if (lodI < rtData->riRes[ri_idx]->getQlBestLod())
          lodI = rtData->riRes[ri_idx]->getQlBestLod();

        int cellAdded = -1;
        for (uint8_t *rangeI = visData.cellRanges.data() + visData.cells[vi].rangesStart,
                     *end = rangeI + visData.cells[vi].rangesCount;
             rangeI != end; rangeI += 2)
        {
          const RendInstGenData::CellRtData::SubCellSlice &scss = crt.getCellSlice(ri_idx, rangeI[0]);
          const RendInstGenData::CellRtData::SubCellSlice &scse = crt.getCellSlice(ri_idx, rangeI[1]);
          if (scse.ofs + scse.sz == scss.ofs)
            continue;
          // add to farLodNo
          if (cellAdded < 0)
            cellAdded = visibility.addCell(lodI, x, z, startVbOfs, visData.cells[vi].rangesCount >> 1);
          visibility.addRange(lodI, scss.ofs, (scse.ofs + scse.sz - scss.ofs));
          visibility.instNumberCounter[lodI] += (scse.ofs + scse.sz - scss.ofs) / visibility.stride;
        }
        if (cellAdded >= 0)
          visibility.closeRanges(lodI);
      }
      else
      {
        // separate subcells between different lods
        carray<int, rendinst::render::MAX_LOD_COUNT_WITH_ALPHA> cellAdded;
        int lastLod = -1;
        mem_set_ff(cellAdded);
        for (uint8_t *rangeI = visData.cellRanges.data() + visData.cells[vi].rangesStart,
                     *end = rangeI + visData.cells[vi].rangesCount;
             rangeI != end; rangeI += 2)
        {
          const RendInstGenData::CellRtData::SubCellSlice &scss = crt.getCellSlice(ri_idx, rangeI[0]);
          const RendInstGenData::CellRtData::SubCellSlice &scse = crt.getCellSlice(ri_idx, rangeI[1]);
          if (scse.ofs + scse.sz == scss.ofs)
            continue;
          for (int cri = rangeI[0]; cri <= rangeI[1]; ++cri)
          {
            const RendInstGenData::CellRtData::SubCellSlice &sc = crt.getCellSlice(ri_idx, cri);
            if (!sc.sz) // empty subcell
              continue;

            float subCellDistSq = v_extract_x(v_distance_sq_to_bbox_x(crt.bbox[cri + 1].bmin, crt.bbox[cri + 1].bmax, curViewPos));

            float maxDistSq = visibility.forcedLod < 0 ? farLodEndRangeSq : forcedLodDistSq;
            if (subCellDistSq >= maxDistSq)
              continue; // too far away

            if (use_external_filter)
            {
              if (!external_filter(crt.bbox[cri + 1].bmin, crt.bbox[cri + 1].bmax))
                continue;
            }
            if (vi < MAX_PER_INSTANCE_CELLS && pool.hasImpostor() && rendinst::render::per_instance_visibility) // todo: support not
                                                                                                                // impostors
            {
              if (subCellDistSq < distanceToCheckPerInstanceSq)
              {
                for (int16_t *data = (int16_t *)(crt.sysMemData + sc.ofs), *data_e = data + sc.sz / 2; data < data_e; data += 4)
                {
                  if (rendinst::is_pos_rendinst_data_destroyed(data))
                    continue;
                  bool palette_rotation = rtData->riPaletteRotation[ri_idx];
                  vec4f v_pos, v_scale;
                  vec4i v_palette_id;
                  rendinst::gen::unpack_tm_pos(v_pos, v_scale, data, v_cell_add[vi], v_cell_mul[vi], palette_rotation, &v_palette_id);
                  bbox3f treeBBox;
                  if (palette_rotation)
                  {
                    quat4f v_rot = rendinst::gen::RotationPaletteManager::get_quat(palette, v_extract_xi(v_palette_id));
                    mat44f tm;
                    v_mat44_compose(tm, v_pos, v_rot, v_scale);
                    bbox3f bbox;
                    bbox.bmin = v_tree_min;
                    bbox.bmax = v_tree_max;
                    v_bbox3_init(treeBBox, tm, bbox);
                  }
                  else
                  {
                    treeBBox.bmin = v_add(v_pos, v_mul(v_scale, v_tree_min));
                    treeBBox.bmax = v_add(v_pos, v_mul(v_scale, v_tree_max));
                  }
                  if (frustum.testBoxExtentB(v_add(treeBBox.bmax, treeBBox.bmin), v_sub(treeBBox.bmax, treeBBox.bmin)))
                  {
                    if (use_occlusion)
                    {
                      vec3f occBmin = treeBBox.bmin, occBmax = treeBBox.bmax;
                      if (forShadow)
                      {
                        const float maxLightDistForTreeShadow = 20.0f;
                        vec3f lightDist = v_mul(v_splats((maxLightDistForTreeShadow * 2.0)),
                          reinterpret_cast<vec4f &>(rendinst::render::dir_from_sun));
                        vec3f far_point = v_mul(v_add(v_add(treeBBox.bmax, treeBBox.bmin), lightDist), V_C_HALF);
                        occBmin = v_min(occBmin, far_point);
                        occBmax = v_max(occBmax, far_point);
                      }

                      if (pool.hasImpostor())
                      {
                        // impostor is offsetted by cylinder_radius in vshader so check sphere occlusion instead
                        vec3f sphCenter = v_add(v_pos, v_make_vec4f(0.0f, pool.sphCenterY, 0.0f, 0.0f));
                        vec4f radius = v_mul(v_splats(pool.sphereRadius), v_scale);
                        if (use_occlusion->isOccludedSphere(sphCenter, radius))
                          continue;
                      }
                      else if (use_occlusion->isOccludedBox(occBmin, occBmax))
                        continue;
                    }
                    int lodI;
                    float instance_dist2 = crossDissolveForPool || pool.hasTransitionLod()
                                             ? v_extract_x(v_length3_sq_x(v_sub(v_pos, curViewPos)))
                                             : v_extract_x(v_distance_sq_to_bbox_x(treeBBox.bmin, treeBBox.bmax, curViewPos));

                    for (lodI = visibility.PI_ALPHA_LOD; lodI > 0; --lodI) // alphaFarLodNo!
                      if (lodI != visibility.PI_DISSOLVE_LOD0 && instance_dist2 > lodDistancesSq_perInst[lodI])
                        break;
                    rtData->riRes[ri_idx]->updateReqLod(min<int>(lodI, rtData->riRes[ri_idx]->lods.size() - 1));
                    if (lodI < rtData->riRes[ri_idx]->getQlBestLod())
                      lodI = rtData->riRes[ri_idx]->getQlBestLod();

                    vec4f v_packed_data = rendinst::gen::pack_entity_data(v_pos, v_scale, v_palette_id);
                    int instanceLod = lodI == visibility.PI_DISSOLVE_LOD1 ? visibility.PI_LAST_MESH_LOD : lodI;
                    if (lodI == RiGenVisibility::PI_DISSOLVE_LOD1 || lodI == RiGenVisibility::PI_DISSOLVE_LOD0)
                    {
                      if (lodI == visibility.PI_DISSOLVE_LOD1)
                      {
                        visibility.addTreeInstance(ri_idx, perInstanceData[RiGenVisibility::PI_DISSOLVE_LOD0], v_packed_data,
                          RiGenVisibility::PI_DISSOLVE_LOD0);
                        visibility.addTreeInstance(ri_idx, perInstanceData[RiGenVisibility::PI_DISSOLVE_LOD1], v_packed_data,
                          RiGenVisibility::PI_DISSOLVE_LOD1);
                      }
                    }
                    else if (pool_front_to_back && (lodI <= RiGenVisibility::PI_DISSOLVE_LOD1) &&
                             perInstanceDistanceCnt[instanceLod] < perInstanceDistance[instanceLod].size())
                    {
                      sortedInstances[instanceLod][perInstanceDistanceCnt[instanceLod]] = v_packed_data;
                      int distance = bitwise_cast<int, float>(instance_dist2);

                      if (forShadow) // we shall sort from front to back
                        distance = v_extract_xi(v_cvt_vec4i(v_dot3(((vec4f &)rendinst::render::dir_from_sun), v_pos)));

                      perInstanceDistance[instanceLod][perInstanceDistanceCnt[instanceLod]] =
                        IPoint2(perInstanceDistanceCnt[instanceLod], distance);
                      perInstanceDistanceCnt[instanceLod]++;
                      if (lodI == visibility.PI_DISSOLVE_LOD1)
                      {
                        visibility.addTreeInstance(ri_idx, perInstanceData[lodI], v_packed_data, lodI); // lod 1 dissappear, lod0 as is
                      }
                    }
                    else
                    {
                      visibility.addTreeInstance(ri_idx, perInstanceData[lodI], v_packed_data, lodI);
                      if (lodI == visibility.PI_DISSOLVE_LOD1)
                        visibility.addTreeInstance(ri_idx, perInstanceData[visibility.PI_LAST_MESH_LOD], v_packed_data,
                          visibility.PI_LAST_MESH_LOD); // lod 1 dissappear, lod0 as is
                      else if (lodI == visibility.PI_DISSOLVE_LOD0)
                        visibility.addTreeInstance(ri_idx, perInstanceData[visibility.PI_IMPOSTOR_LOD], v_packed_data,
                          visibility.PI_IMPOSTOR_LOD); // lod 0 dissappear, lod1 as is
                    }

                    visibility.instNumberCounter[remap_per_instance_lod_inv(lodI)]++;
                    if (lodI >= RiGenVisibility::PI_IMPOSTOR_LOD) // PI_ALPHA_LOD also requires an impostor texture.
                    {
                      riRenderRanges[ri_idx].vismask |= VIS_HAS_IMPOSTOR;
                    }
                  }
                }
                continue;
              }
            }
            // todo:replace to binary search?
            for (int lodI = pool.hasImpostor() ? farLodNo : alphaFarLodNo, lodE = rtData->riResFirstLod(ri_idx); lodI >= lodE; --lodI)
            {
              if (subCellDistSq > lodDistancesSq[lodI + lodTranslation] || lodI == lodE)
              {
                if (forShadow && lodI == alphaFarLodNo)
                  break;
                // add range to lodI
                if (cellAdded[lodI] < 0)
                  cellAdded[lodI] = visibility.addCell(lastLod = lodI, x, z, startVbOfs);
                visibility.addRange(lodI, sc.ofs, sc.sz);
                G_ASSERT(sc.sz % visibility.stride == 0);
                visibility.instNumberCounter[lodI] += sc.sz / visibility.stride;
                break;
              }
            }
          }
        }
        if (lastLod >= 0)
          visibility.closeRanges(lastLod);
      }
      for (int lodI = 0; lodI < perInstanceDistanceCnt.size(); ++lodI)
      {
        if (!perInstanceDistanceCnt[lodI])
          continue;
        stlsort::sort(perInstanceDistance[lodI].data(), perInstanceDistance[lodI].data() + perInstanceDistanceCnt[lodI], SortByY());
        int perInstanceStart = visibility.addTreeInstances(ri_idx, perInstanceData[lodI], nullptr, perInstanceDistanceCnt[lodI], lodI);
        vec4f *dest = &visibility.instanceData[lodI][perInstanceStart];
        for (int i = 0; i < perInstanceDistanceCnt[lodI]; ++i, dest++)
          *dest = sortedInstances[lodI][perInstanceDistance[lodI][i].x];
      }
    }
    for (int lodI = 0; lodI < visibility.instanceData.size(); ++lodI)
    {
      visibility.max_per_instance_instances =
        max(visibility.max_per_instance_instances, (int)visibility.instanceData[lodI].size() - prevInstancesCount[lodI]);
      prevInstancesCount[lodI] = visibility.instanceData[lodI].size();
    }
    visibility.endRenderRange(ri_idx);
    riRenderRanges[ri_idx].vismask = 0;
    for (int lodI = 0; lodI < alphaFarLodNo; ++lodI)
    {
      if (riRenderRanges[ri_idx].hasCells(lodI))
      {
        riRenderRanges[ri_idx].vismask |= VIS_HAS_OPAQUE;
        break;
      }
    }
    riRenderRanges[ri_idx].vismask |= riRenderRanges[ri_idx].hasCells(alphaFarLodNo) ? VIS_HAS_TRANSP : 0;

    if (pool.hasImpostor())
    {
      riRenderRanges[ri_idx].vismask |=
        ((riRenderRanges[ri_idx].vismask & VIS_HAS_TRANSP) || riRenderRanges[ri_idx].hasCells(alphaFarLodNo - 1) ||
          visibility.perInstanceVisibilityCells[visibility.PI_IMPOSTOR_LOD].size() > 0 ||
          visibility.perInstanceVisibilityCells[visibility.PI_ALPHA_LOD].size() > 0) // PI_ALPHA_LOD also requires an impostor texture.
          ? VIS_HAS_IMPOSTOR
          : 0;
    }

    visibility.vismask |= riRenderRanges[ri_idx].vismask;
  }

  if (rendinst::render::per_instance_visibility)
  {
    visibility.closeTreeInstances();
    visibility.vismask |= (visibility.perInstanceVisibilityCells[visibility.PI_ALPHA_LOD].size() > 0) ? VIS_HAS_TRANSP : 0;
    visibility.vismask |=
      ((visibility.vismask & VIS_HAS_TRANSP) || visibility.perInstanceVisibilityCells[visibility.PI_IMPOSTOR_LOD].size() > 0)
        ? VIS_HAS_IMPOSTOR
        : 0;
    for (int i = 0; i < visibility.PI_ALPHA_LOD; ++i)
      visibility.vismask |= (visibility.perInstanceVisibilityCells[i].size() > 0) ? VIS_HAS_OPAQUE : 0;
  }
  return visibility.vismask != 0;
}

// Explicit instantiation of all specialization, cuz we use them in other translation units.
template bool RendInstGenData::prepareVisibility<true>(const Frustum &, const Point3 &, RiGenVisibility &, bool, rendinst::LayerFlags,
  Occlusion *, bool, const rendinst::VisibilityExternalFilter &);
template bool RendInstGenData::prepareVisibility<false>(const Frustum &, const Point3 &, RiGenVisibility &, bool, rendinst::LayerFlags,
  Occlusion *, bool, const rendinst::VisibilityExternalFilter &);
