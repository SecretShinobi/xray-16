#include "stdafx.h"

void CRender::render_lights(light_Package& LP)
{
    //////////////////////////////////////////////////////////////////////////
    // Refactor order based on ability to pack shadow-maps
    // 1. calculate area + sort in descending order
    // const	u16		smap_unassigned		= u16(-1);
    {
        xr_vector<light*>& source = LP.v_shadowed;
        for (u32 it = 0; it < source.size(); it++)
        {
            light* L = source[it];
            L->vis_update();
            if (!L->vis.visible)
            {
                source.erase(source.begin() + it);
                it--;
            }
            else
            {
                LR.compute_xf_spot(L);
            }
        }
    }

    // 2. refactor - infact we could go from the backside and sort in ascending order
    {
        xr_vector<light*>& source = LP.v_shadowed;
        xr_vector<light*> refactored;
        refactored.reserve(source.size());
        const size_t total = source.size();

        for (u16 smap_ID = 0; refactored.size() != total; ++smap_ID)
        {
            LP_smap_pool.initialize(RImplementation.o.smapsize);
            std::sort(source.begin(), source.end(), [](light* l1, light* l2)
            {
                const u32 a0 = l1->X.S.size;
                const u32 a1 = l2->X.S.size;
                return a0 > a1; // reverse -> descending
            });
            for (size_t test = 0; test < source.size(); ++test)
            {
                light* L = source[test];
                SMAP_Rect R{};
                if (LP_smap_pool.push(R, L->X.S.size))
                {
                    // OK
                    L->X.S.posX = R.min.x;
                    L->X.S.posY = R.min.y;
                    L->vis.smap_ID = smap_ID;
                    refactored.push_back(L);
                    source.erase(source.begin() + test);
                    --test;
                }
            }
        }

        // save (lights are popped from back)
        std::reverse(refactored.begin(), refactored.end());
        LP.v_shadowed = std::move(refactored);
    }

    PIX_EVENT(SHADOWED_LIGHTS);

    //////////////////////////////////////////////////////////////////////////
    // sort lights by importance???
    // while (has_any_lights_that_cast_shadows) {
    //		if (has_point_shadowed)		->	generate point shadowmap
    //		if (has_spot_shadowed)		->	generate spot shadowmap
    //		switch-to-accumulator
    //		if (has_point_unshadowed)	-> 	accum point unshadowed
    //		if (has_spot_unshadowed)	-> 	accum spot unshadowed
    //		if (was_point_shadowed)		->	accum point shadowed
    //		if (was_spot_shadowed)		->	accum spot shadowed
    //	}
    //	if (left_some_lights_that_doesn't cast shadows)
    //		accumulate them
    HOM.Disable();
    while (!LP.v_shadowed.empty())
    {
        // if (has_spot_shadowed)
        xr_vector<light*> L_spot_s;
        Stats.s_used++;

        // generate spot shadowmap
        Target->phase_smap_spot_clear();
        xr_vector<light*>& source = LP.v_shadowed;
        light* L = source.back();
        u16 sid = L->vis.smap_ID;
        while (true)
        {
            if (source.empty())
                break;
            L = source.back();
            if (L->vis.smap_ID != sid)
                break;
            source.pop_back();
            Lights_LastFrame.push_back(L);

            // render
            phase = PHASE_SMAP;
            if (RImplementation.o.Tshadows)
                r_pmask(true, true);
            else
                r_pmask(true, false);
            L->svis.begin();
            PIX_EVENT(SHADOWED_LIGHTS_RENDER_SUBSPACE);
            r_dsgraph_render_subspace(L->spatial.sector, L->X.S.combine, L->position, TRUE);
            bool bNormal = !mapNormalPasses[0][0].empty() || !mapMatrixPasses[0][0].empty();
            bool bSpecial = !mapNormalPasses[1][0].empty() || !mapMatrixPasses[1][0].empty() || !mapSorted.empty();
            if (bNormal || bSpecial)
            {
                Stats.s_merged++;
                L_spot_s.push_back(L);
                Target->phase_smap_spot(L);
                RCache.set_xform_world(Fidentity);
                RCache.set_xform_view(L->X.S.view);
                RCache.set_xform_project(L->X.S.project);
                r_dsgraph_render_graph(0);
                if (ps_r2_ls_flags.test(R2FLAG_SUN_DETAILS))
                    Details->Render();
                L->X.S.transluent = FALSE;
                if (bSpecial)
                {
                    L->X.S.transluent = TRUE;
                    Target->phase_smap_spot_tsh(L);
                    PIX_EVENT(SHADOWED_LIGHTS_RENDER_GRAPH);
                    r_dsgraph_render_graph(1); // normal level, secondary priority
                    PIX_EVENT(SHADOWED_LIGHTS_RENDER_SORTED);
                    r_dsgraph_render_sorted(); // strict-sorted geoms
                }
            }
            else
            {
                Stats.s_finalclip++;
            }
            L->svis.end();
            r_pmask(true, false);
        }

        PIX_EVENT(UNSHADOWED_LIGHTS);

        //		switch-to-accumulator
        Target->phase_accumulator();
        HOM.Disable();

        PIX_EVENT(POINT_LIGHTS);

        //		if (has_point_unshadowed)	-> 	accum point unshadowed
        if (!LP.v_point.empty())
        {
            light* L2 = LP.v_point.back();
            LP.v_point.pop_back();
            L2->vis_update();
            if (L2->vis.visible)
            {
                Target->accum_point(L2);
                render_indirect(L2);
            }
        }

        PIX_EVENT(SPOT_LIGHTS);

        //		if (has_spot_unshadowed)	-> 	accum spot unshadowed
        if (!LP.v_spot.empty())
        {
            light* L2 = LP.v_spot.back();
            LP.v_spot.pop_back();
            L2->vis_update();
            if (L2->vis.visible)
            {
                LR.compute_xf_spot(L2);
                Target->accum_spot(L2);
                render_indirect(L2);
            }
        }

        PIX_EVENT(SPOT_LIGHTS_ACCUM_VOLUMETRIC);

        //		if (was_spot_shadowed)		->	accum spot shadowed
        if (!L_spot_s.empty())
        {
            PIX_EVENT(ACCUM_SPOT);
            for (auto& p_light : L_spot_s)
            {
                Target->accum_spot(p_light);
                render_indirect(p_light);
            }

            PIX_EVENT(ACCUM_VOLUMETRIC);
            if (RImplementation.o.advancedpp && ps_r2_ls_flags.is(R2FLAG_VOLUMETRIC_LIGHTS))
                for (auto& p_light : L_spot_s)
                    Target->accum_volumetric(p_light);

            L_spot_s.clear();
        }
    }

    PIX_EVENT(POINT_LIGHTS_ACCUM);
    // Point lighting (unshadowed, if left)
    if (!LP.v_point.empty())
    {
        xr_vector<light*>& Lvec = LP.v_point;
        for (auto & p_light : Lvec)
        {
            p_light->vis_update();
            if (p_light->vis.visible)
            {
                render_indirect(p_light);
                Target->accum_point(p_light);
            }
        }
        Lvec.clear();
    }

    PIX_EVENT(SPOT_LIGHTS_ACCUM);
    // Spot lighting (unshadowed, if left)
    if (!LP.v_spot.empty())
    {
        xr_vector<light*>& Lvec = LP.v_spot;
        for (auto& p_light : Lvec)
        {
            p_light->vis_update();
            if (p_light->vis.visible)
            {
                LR.compute_xf_spot(p_light);
                render_indirect(p_light);
                Target->accum_spot(p_light);
            }
        }
        Lvec.clear();
    }
}

void CRender::render_indirect(light* L)
{
    if (!ps_r2_ls_flags.test(R2FLAG_GI))
        return;

    light LIGEN;
    LIGEN.set_type(IRender_Light::REFLECTED);
    LIGEN.set_shadow(false);
    LIGEN.set_cone(PI_DIV_2 * 2.f);

    xr_vector<light_indirect>& Lvec = L->indirect;
    if (Lvec.empty())
        return;
    float LE = L->color.intensity();
    for (auto& LI : Lvec)
    {
        // energy and color
        float LIE = LE * LI.E;
        if (LIE < ps_r2_GI_clip)
            continue;
        Fvector T;
        T.set(L->color.r, L->color.g, L->color.b).mul(LI.E);
        LIGEN.set_color(T.x, T.y, T.z);

        // geometric
        Fvector L_up, L_right;
        L_up.set(0, 1, 0);
        if (_abs(L_up.dotproduct(LI.D)) > .99f)
            L_up.set(0, 0, 1);
        L_right.crossproduct(L_up, LI.D).normalize();
        LIGEN.spatial.sector = LI.S;
        LIGEN.set_position(LI.P);
        LIGEN.set_rotation(LI.D, L_right);

        // range
        // dist^2 / range^2 = A - has infinity number of solutions
        // approximate energy by linear fallof Emax / (1 + x) = Emin
        float Emax = LIE;
        float Emin = 1.f / 255.f;
        float x = (Emax - Emin) / Emin;
        if (x < 0.1f)
            continue;
        LIGEN.set_range(x);

        Target->accum_reflected(&LIGEN);
    }
}
