#include "ForgeCloud.hpp"

#include <cstdlib>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <fstream>

#include <wx/string.h>
#include <wx/utils.h>   // wxExecute
#include <wx/arrstr.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "I18N.hpp"
#include "../Utils/ForgeCloudAgent.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/ForgePrinterMatch.hpp"
#include "libslic3r/Utils.hpp" // data_dir

#include <nlohmann/json.hpp>

#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/msgdlg.h>
#include <boost/filesystem.hpp>

namespace Slic3r { namespace GUI {

std::string forge_dashboard_url()
{
    if (AppConfig* cfg = wxGetApp().app_config) {
        std::string v = cfg->get("forge_dashboard_url");
        if (!v.empty()) return v;
    }
    if (const char* env = std::getenv("FORGE_DASHBOARD_URL"); env && *env)
        return env;
    if (AppConfig* cfg = wxGetApp().app_config) {
        std::string v = cfg->get("forge_server_url"); // fleet panel's key
        if (!v.empty()) return v;
    }
    // Default to the dashboard's plain-HTTP API port. The embedded HTTP client
    // has no TLS, and the API answers on http://…:3000 with 200 (no forced
    // HTTPS redirect for /api paths) — matching the fleet agent's own default,
    // so spool/currency/send features work out of the box without configuration.
    return "http://127.0.0.1:3000";
}

void set_forge_dashboard_url(const std::string& url)
{
    if (AppConfig* cfg = wxGetApp().app_config) {
        cfg->set("forge_dashboard_url", url);
        cfg->set("forge_server_url", url); // keep the fleet agent in sync
    }
}

// Internal alias used by the provider below.
static std::string dashboard_url() { return forge_dashboard_url(); }

// Minimal URL-encoding for query values (filenames may contain spaces).
static std::string url_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 3DPrintForge dashboard provider.
//
// HTTP is bridged through curl via wxExecute so we don't pull OpenSSL into
// the GUI just to call our own dashboard (same approach as ForgeLibraryDialog;
// Phase 3 will swap this for cpp-httplib + OpenSSL once that is wired in).
// ---------------------------------------------------------------------------
class ThreeDPrintForgeProvider : public CloudProvider
{
public:
    std::string id() const override { return "3dprintforge"; }
    std::string display_name() const override { return "3DPrintForge"; }
    // Always available — defaults to the local dashboard.
    bool        is_configured() const override { return true; }

    CloudJobResult send_job(const std::string& file_path,
                            const std::string& filename,
                            const std::string& printer_id,
                            bool               auto_queue) override
    {
        CloudJobResult r;

        std::string url = dashboard_url() + "/api/slicer/upload?filename=" + url_encode(filename);
        if (!printer_id.empty())
            url += "&printer_id=" + url_encode(printer_id);
        if (auto_queue)
            url += "&auto_queue=1";

        // Authenticate with the stored login session so the upload works on
        // dashboards that require auth (the server accepts the bambu_session
        // cookie or a Bearer token). Empty when not signed in -> sent open,
        // which is fine for an auth-disabled dashboard.
        std::string auth_args;
        if (AppConfig* cfg = wxGetApp().app_config) {
            const std::string token = cfg->get("forge_session_token");
            if (!token.empty()) {
                auth_args = wxString::Format(
                    " -H \"Cookie: bambu_session=%s\" -H \"Authorization: Bearer %s\"",
                    wxString::FromUTF8(token.c_str()),
                    wxString::FromUTF8(token.c_str())).ToStdString();
            }
        }

        // -k: accept the dashboard's self-signed cert. --data-binary @file
        // streams the raw bytes the upload endpoint expects.
        wxString cmd = wxString::Format(
            "curl -ks -X POST \"%s\"%s --data-binary @\"%s\" --max-time 120",
            wxString::FromUTF8(url.c_str()),
            wxString::FromUTF8(auth_args.c_str()),
            wxString::FromUTF8(file_path.c_str()));

        wxArrayString out, err;
        long exit_code = wxExecute(cmd, out, err, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
        if (exit_code != 0) {
            r.ok = false;
            r.message = "curl exit code " + std::to_string(exit_code) +
                        " (is 3DPrintForge running at " + dashboard_url() + "?)";
            return r;
        }

        // The endpoint replies 201 with a JSON body; we don't parse it here
        // (no JSON lib on the GUI side yet) — a zero exit means accepted.
        r.ok = true;
        if (!out.IsEmpty())
            r.message = out[0].ToStdString();
        return r;
    }
};

// ---------------------------------------------------------------------------
// Print-time "Send to 3DPrintForge" (brand-agnostic, via the dashboard fleet)
// ---------------------------------------------------------------------------
void forge_pick_printer_and_send()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;

    PartPlate* plate = plater->get_partplate_list().get_curr_plate();
    if (plate == nullptr || !plate->is_slice_result_valid()) {
        wxMessageBox(_L("Slice the plate first, then send it to a printer."),
                     _L("Send to 3DPrintForge"), wxICON_INFORMATION);
        return;
    }

    // Fetch the fleet from the dashboard so the user picks from THEIR printers.
    ForgeCloudAgent agent;
    agent.set_server_url(forge_dashboard_url());
    std::vector<ForgePrinter> printers = agent.list_printers();
    if (printers.empty()) {
        wxMessageBox(wxString::Format(
                         _L("No printers found. Add one in Devices, and check the server URL (%s)."),
                         wxString::FromUTF8(forge_dashboard_url())),
                     _L("Send to 3DPrintForge"), wxICON_INFORMATION);
        return;
    }

    // Printer picker.
    wxDialog dlg(plater, wxID_ANY, _L("Send to 3DPrintForge"));
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(&dlg, wxID_ANY, _L("Choose a printer:")), 0, wxALL, 12);
    auto* choice = new wxChoice(&dlg, wxID_ANY);
    for (const auto& p : printers) {
        wxString label = wxString::FromUTF8(p.name);
        if (!p.status.empty()) label += " (" + wxString::FromUTF8(p.status) + ")";
        choice->Append(label);
    }
    choice->SetSelection(0);
    root->Add(choice, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);
    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer();
    btns->Add(new wxButton(&dlg, wxID_OK,     _L("Send")),   0, wxRIGHT, 8);
    btns->Add(new wxButton(&dlg, wxID_CANCEL, _L("Cancel")), 0);
    root->Add(btns, 0, wxEXPAND | wxALL, 12);
    dlg.SetSizerAndFit(root);
    if (dlg.ShowModal() != wxID_OK)
        return;

    const int sel = choice->GetSelection();
    if (sel < 0 || sel >= (int)printers.size())
        return;
    const std::string printer_id   = printers[sel].id;
    const wxString    printer_name = wxString::FromUTF8(printers[sel].name);

    // Export the sliced plate, upload, queue.
    namespace fs = boost::filesystem;
    const int plate_idx = plater->get_partplate_list().get_curr_plate_index();
    wxString fname_wx = plater->get_export_gcode_filename(".gcode.3mf", /*only_filename*/ true);
    std::string fname = fname_wx.empty() ? std::string("print.gcode.3mf") : std::string(fname_wx.ToUTF8());
    fs::path out_path = fs::temp_directory_path() / ("forge_" + std::to_string(plate_idx) + "_" + fname);

    CloudJobResult res;
    {
        wxBusyCursor wait;
        plater->export_3mf(out_path,
                          SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode,
                          plate_idx);
        boost::system::error_code ec;
        if (!fs::exists(out_path, ec) || fs::file_size(out_path, ec) == 0) {
            wxMessageBox(_L("Could not export the sliced file."),
                         _L("Send to 3DPrintForge"), wxICON_ERROR);
            return;
        }
        CloudProvider* prov = cloud_provider("3dprintforge");
        res = prov ? prov->send_job(out_path.string(), fname, printer_id, /*auto_queue*/ true)
                   : CloudJobResult{};
        fs::remove(out_path, ec);
    }

    if (res.ok)
        wxMessageBox(wxString::Format(_L("Sent to %s and queued."), printer_name),
                     _L("Send to 3DPrintForge"), wxICON_INFORMATION);
    else
        wxMessageBox(wxString::Format(_L("Send failed: %s"), wxString::FromUTF8(res.message)),
                     _L("Send to 3DPrintForge"), wxICON_ERROR);
}

// Cache of the connected fleet's (vendor, model) pairs, filled on each fleet
// sync, so the Printer combo can mark connected presets without hitting the net.
static std::vector<std::pair<std::string, std::string>> g_forge_fleet_vm;

bool forge_preset_is_connected(const std::string& preset_name)
{
    if (preset_name.empty())
        return false;
    const std::vector<std::string> one{ preset_name };
    for (const auto& vm : g_forge_fleet_vm)
        if (match_fleet_printer_preset(vm.first, vm.second, one) == preset_name)
            return true;
    return false;
}

int forge_sync_fleet_to_presets()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return 0;

    ForgeCloudAgent agent;
    agent.set_server_url(forge_dashboard_url());
    const std::vector<ForgePrinter> fleet = agent.list_printers();
    if (fleet.empty())
        return 0;

    // Refresh the connected-models cache for the Printer-combo markers.
    g_forge_fleet_vm.clear();
    for (const ForgePrinter& fp : fleet)
        if (!fp.model.empty())
            g_forge_fleet_vm.emplace_back(fp.vendor, fp.model);

    // Load the installable catalogue: (vendor folder, machine model, nozzle).
    namespace fs = boost::filesystem;
    const fs::path system_dir = fs::path(Slic3r::data_dir()) / "system";
    if (!fs::exists(system_dir))
        return 0;

    struct CatEntry { std::string vendor, model, nozzle; };
    std::vector<CatEntry>    catalog;
    std::vector<std::string> model_names;
    for (fs::directory_iterator it(system_dir), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path()) || it->path().extension() != ".json")
            continue;
        const std::string vendor = it->path().stem().string();
        if (vendor == "Custom" || vendor == "OrcaFilamentLibrary")
            continue;
        try {
            std::ifstream ifs(it->path().string());
            nlohmann::json vj; ifs >> vj;
            if (!vj.contains("machine_model_list")) continue;
            for (const auto& m : vj["machine_model_list"]) {
                if (!m.contains("name") || !m.contains("sub_path")) continue;
                const std::string model = m["name"].get<std::string>();
                const fs::path mp = system_dir / vendor / m["sub_path"].get<std::string>();
                if (!fs::exists(mp)) continue;
                std::ifstream mifs(mp.string());
                nlohmann::json mj; mifs >> mj;
                if (!mj.contains("nozzle_diameter")) continue;
                const std::string nozzles = mj["nozzle_diameter"].get<std::string>();
                std::string noz;
                for (size_t i = 0; i <= nozzles.size(); ++i) {
                    if (i == nozzles.size() || nozzles[i] == ';') {
                        if (!noz.empty()) { catalog.push_back({vendor, model, noz}); model_names.push_back(model); }
                        noz.clear();
                    } else {
                        noz.push_back(nozzles[i]);
                    }
                }
            }
        } catch (...) { /* skip malformed vendor */ }
    }
    if (catalog.empty())
        return 0;

    int installed = 0;
    for (const ForgePrinter& fp : fleet) {
        if (fp.model.empty()) continue;
        const std::string best_model = match_fleet_printer_preset(fp.vendor, fp.model, model_names);
        if (best_model.empty()) continue;
        // Prefer a 0.4 nozzle variant of the matched model, else the first.
        const CatEntry* chosen = nullptr;
        for (const CatEntry& e : catalog) {
            if (e.model != best_model) continue;
            if (e.nozzle == "0.4") { chosen = &e; break; }
            if (chosen == nullptr) chosen = &e;
        }
        if (chosen == nullptr) continue;
        const std::string variant = chosen->nozzle + " nozzle";
        if (!cfg->get_variant(chosen->vendor, chosen->model, variant)) {
            cfg->set_variant(chosen->vendor, chosen->model, variant, true);
            ++installed;
        }
    }
    if (installed > 0) {
        cfg->save();
        try { wxGetApp().load_current_presets(); } catch (...) { /* user can restart */ }
    }
    return installed;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
std::vector<CloudProvider*> cloud_providers()
{
    static ThreeDPrintForgeProvider s_forge;
    static std::vector<CloudProvider*> s_providers = { &s_forge };
    return s_providers;
}

CloudProvider* cloud_provider(const std::string& id)
{
    for (CloudProvider* p : cloud_providers())
        if (p->id() == id)
            return p;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Profile catalog push (slicer -> dashboard)
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    o += buf;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

CloudJobResult sync_profiles_to_forge()
{
    CloudJobResult r;
    Slic3r::PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr) { r.message = "no preset bundle loaded"; return r; }

    std::string body = "{\"profiles\":[";
    bool first = true;
    auto add = [&](Slic3r::PresetCollection& coll, const char* kind) {
        for (auto it = coll.lbegin(); it != coll.end(); ++it) {
            const Slic3r::Preset& p = *it;
            if (!first) body += ',';
            first = false;
            body += "{\"kind\":\"";
            body += kind;
            body += "\",\"name\":\"";
            body += json_escape(p.name);
            body += "\"}";
        }
    };
    add(pb->printers,  "printer");
    add(pb->filaments, "filament");
    add(pb->prints,    "process");
    body += "]}";

    wxFileName pfn(wxStandardPaths::Get().GetTempDir(), "forge-profiles-push.json");
    const std::string param_path = pfn.GetFullPath().ToStdString();
    { std::ofstream pf(param_path, std::ios::binary); pf << body; }

    const std::string url = forge_dashboard_url() + "/api/slicer/profiles/push";
    wxString cmd = wxString::Format(
        "curl -ks -X POST \"%s\" -H \"Content-Type: application/json\" -d @\"%s\" --max-time 60",
        wxString::FromUTF8(url.c_str()), wxString::FromUTF8(param_path.c_str()));

    wxArrayString out, err;
    long ec = wxExecute(cmd, out, err, wxEXEC_SYNC | wxEXEC_HIDE_CONSOLE);
    if (ec != 0) {
        r.message = "curl exit code " + std::to_string(ec) +
                    " (is 3DPrintForge running at " + forge_dashboard_url() + "?)";
        return r;
    }
    r.ok = true;
    if (!out.IsEmpty()) r.message = out[0].ToStdString();
    return r;
}

}} // namespace Slic3r::GUI
