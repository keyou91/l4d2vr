#pragma once
#include <string_view>
namespace l4d2vr_weapon_models
{
    inline int StockWeaponId(std::string_view path)
    {
        struct Model { std::string_view path; int id; };
        static constexpr Model models[] = {
            {"models/v_models/v_pistola.mdl",1},{"models/v_models/v_dual_pistola.mdl",1},
            {"models/v_models/v_smg.mdl",2},{"models/v_models/v_pumpshotgun.mdl",3},
            {"models/v_models/v_autoshotgun.mdl",4},{"models/v_models/v_rifle.mdl",5},
            {"models/v_models/v_huntingrifle.mdl",6},{"models/v_models/v_silenced_smg.mdl",7},
            {"models/v_models/v_shotgun_chrome.mdl",8},{"models/v_models/v_desert_rifle.mdl",9},
            {"models/v_models/v_sniper_military.mdl",10},{"models/v_models/v_shotgun_spas.mdl",11},
            {"models/v_models/v_grenade_launcher.mdl",21},{"models/v_models/v_rifle_ak47.mdl",26},
            {"models/v_models/v_desert_eagle.mdl",32},{"models/v_models/v_smg_mp5.mdl",33},
            {"models/v_models/v_rif_sg552.mdl",34},{"models/v_models/v_snip_awp.mdl",35},
            {"models/v_models/v_snip_scout.mdl",36},{"models/v_models/v_m60.mdl",37},
        };
        for (const auto& model : models) if (path == model.path) return model.id;
        return 0;
    }
}
