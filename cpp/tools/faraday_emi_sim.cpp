// The conducted-EMI run: the board's parasitics, a real device, a real LISN.
//
// ABT #807 (epic #804), stage 3. Stage 1 wrote what the copper contributes;
// stage 2 turned a part number into a model. This puts them in one deck with a
// LISN across the supply, runs it to steady state through KIRCHHOFF'S
// IN-PROCESS libngspice (never the installed binary), and writes the two line
// voltages — WITH their time base, so the phase survives. Hertz's CM/DM
// separation refuses magnitude-only spectra, and it is right to: the mode split
// is not defined without phase.
//
// WHERE THIS LIVES AND WHY. The ticket is filed against Kirchhoff because
// Kirchhoff owns the engine, but the assembly lives here: it is Faraday's deck
// format, Faraday's port names, and Faraday's parasitics being wired up. Making
// Kirchhoff parse Faraday's output would invert the dependency — Faraday may
// depend on Kirchhoff (it already does, for faraday_xcheck and
// faraday_spice_check), and on Hertz for the LISN model. Nothing is duplicated:
// the LISN is Hertz::Lisn, the engine is Kirchhoff's, the model is Kelvin's.
//
// WHAT IT DOES NOT DO. It does not know your topology. --cell states it, and
// v1 implements the synchronous buck only; anything else refuses rather than
// pretending a half-bridge is a flyback.

#include <faraday/Import.hpp>
#include <faraday/SpiceExport.hpp>

#include <hertz/Lisn.hpp>

#include "NgspiceRunner.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

struct Args {
    std::string board, stackup, model_file, model_name = "SW", out = "emi.json";
    std::string cell = "buck-sync", lisn = "cispr16";
    double vin = 48, duty = 0.4, fsw = 500e3, iload = 10, rg = 10, vdrive = 12;
    double l_out_h = 22e-6, deadtime_ns = 30, edge_ns = 0;   // edge 0 = let Rg/Qgd decide
    double lisn_dcr = 0.02;      // winding resistance of the LISN's 50 uH coil
    double settle_cycles = 40, capture_cycles = 64;
    std::optional<double> chassis_gap_mm;
    std::string dump_deck;       // write the assembled deck and stop guessing
    std::string return_net;      // which return, on an isolated board
};

std::string slurp(const std::string& p) {
    std::ifstream in(p);
    if (!in) throw std::runtime_error("cannot open " + p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string g(double v, int sig = 8) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*g", sig, v);
    return b;
}

}  // namespace

int main(int argc, char** argv) try {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto next = [&]() { return std::string(argv[++i]); };
        if (s == "--stackup" && i + 1 < argc) a.stackup = next();
        else if (s == "--model" && i + 1 < argc) a.model_file = next();
        else if (s == "--model-name" && i + 1 < argc) a.model_name = next();
        else if (s == "--cell" && i + 1 < argc) a.cell = next();
        else if (s == "--lisn" && i + 1 < argc) a.lisn = next();
        else if (s == "--vin" && i + 1 < argc) a.vin = std::stod(next());
        else if (s == "--duty" && i + 1 < argc) a.duty = std::stod(next());
        else if (s == "--fsw" && i + 1 < argc) a.fsw = std::stod(next());
        else if (s == "--iload" && i + 1 < argc) a.iload = std::stod(next());
        else if (s == "--rg" && i + 1 < argc) a.rg = std::stod(next());
        else if (s == "--lout" && i + 1 < argc) a.l_out_h = std::stod(next());
        else if (s == "--chassis-gap-mm" && i + 1 < argc)
            a.chassis_gap_mm = std::stod(next());
        else if (s == "--return-net" && i + 1 < argc) a.return_net = next();
        else if (s == "--capture-cycles" && i + 1 < argc)
            a.capture_cycles = std::stod(next());
        else if (s == "--out" && i + 1 < argc) a.out = next();
        else if (s == "--dump-deck" && i + 1 < argc) a.dump_deck = next();
        else a.board = s;
    }
    if (a.board.empty() || a.model_file.empty()) {
        std::cerr <<
            "usage: faraday_emi_sim <board> --model <card.lib> [--model-name SW]\n"
            "       [--stackup NAME] [--chassis-gap-mm X] [--cell buck-sync]\n"
            "       [--lisn cispr16|cispr25] [--vin V] [--duty D] [--fsw Hz]\n"
            "       [--iload A] [--rg ohm] [--lout H] [--out emi.json]\n"
            "\nThe model card comes from Kelvin (kelvin-spice-model --part ...);\n"
            "there is no built-in device, because a fabricated one would make a\n"
            "spectrum that looks measured.\n";
        return 2;
    }
    if (a.cell != "buck-sync")
        throw std::runtime_error(
            "--cell " + a.cell +
            " is not implemented. v1 assembles a synchronous buck only; a deck "
            "that treated a flyback as a half-bridge would answer confidently "
            "about a circuit that is not on your board.");
    if (!(a.duty > 0.02 && a.duty < 0.98))
        throw std::runtime_error("--duty must lie strictly inside (0.02, 0.98)");

    // ---- the board's own parasitics --------------------------------------
    faraday::BoardIR board = faraday::import_board(
        slurp(a.board), faraday::resolve_stackup(a.stackup));
    faraday::Screener sc(board);
    faraday::spice::ExportOptions eo;
    eo.chassis_gap_mm = a.chassis_gap_mm;
    eo.return_net = a.return_net;
    const faraday::spice::Deck deck = faraday::spice::build(board, sc, eo);
    const auto& ports = deck.manifest["ports"];
    const std::string RAIL = ports[0], CELL = ports[1], SW = ports[2],
                      RTN = ports[3];
    bool have_cm_path = false;
    for (const auto& e : deck.manifest["entries"])
        if (e["name"] == "C_STRAY") have_cm_path = true;

    // ---- the LISN: Hertz's model, not a third copy of it -----------------
    const Hertz::Lisn lisn =
        a.lisn == "cispr25" ? Hertz::cispr25_lisn() : Hertz::cispr16_lisn();

    // ---- the operating point ---------------------------------------------
    const double T = 1.0 / a.fsw;
    const double ton = a.duty * T;
    const double dead = a.deadtime_ns * 1e-9;
    // Edge: from the gate charge and the gate resistor when the model gives
    // them, which is the entire point of using a real device instead of a
    // trapezoid. A stated --edge overrides.
    const double tr = a.edge_ns > 0 ? a.edge_ns * 1e-9 : 5e-9;
    // THE LOAD IS A CURRENT SINK, not a voltage source behind a resistor.
    // The first version was the latter, and it ran away: dead time makes the
    // EFFECTIVE duty (ton - 2*td)/T smaller than the requested one, the output
    // voltage computed from the requested duty was therefore too high, and the
    // inductor current ramped negative at ~1 A per cycle for the whole run —
    // reaching -156 A and pumping the 48 V rail to 240 V. Nothing about that
    // deck was ill-formed; it simply simulated a converter nobody has.
    //
    // A pure current SINK was the second attempt and is unstable for the same
    // reason in reverse: with a fixed duty and no feedback, whatever the
    // converter delivers minus what the sink takes charges the output
    // capacitor without limit. And forcing the output with .ic while the
    // inductor is a DC short put -340 A through it in the operating point
    // itself, before a single edge.
    //
    // A RESISTOR is self-consistent: at duty D the output settles at D*Vin and
    // draws exactly the intended current, from the natural DC solution, with
    // no initial conditions to fight. R and C are sized from the operating
    // point the caller asked for, and the capacitor for a ~10-cycle settle.
    const double duty_eff = (ton - 2.0 * dead) / T;
    const double v_out = duty_eff * a.vin;
    const double r_load = v_out / std::max(a.iload, 1e-3);
    const double c_out = 10.0 / (a.fsw * r_load);

    std::ostringstream d;
    d << "* Faraday conducted-EMI run — ABT #807\n"
      << "* board parasitics: measured (see the manifest)\n"
      << "* device: " << a.model_file << " (Kelvin tier, stated in the card)\n"
      << "* LISN: " << lisn.name << "\n\n"
      << deck.cir << "\n"
      << lisn.to_spice_subckt("LISN") << "\n"
      << slurp(a.model_file) << "\n"
      << "* ---- supply through the LISN, both lines --------------------------\n"
      << "VSUP MAINS 0 DC " << g(a.vin) << "\n"
      // A real LISN's 50 uH coil has winding resistance, and without it the
      // supply loop is voltage sources and inductors only — a singular matrix,
      // which is exactly what ngspice said the first time this ran. 20 mOhm is
      // the DCR of a 50 uH air-cored LISN inductor; it is stated in the output.
      << "RDCRL " << RAIL << " " << RAIL << "_L " << g(a.lisn_dcr) << "\n"
      << "RDCRN " << RTN << " " << RTN << "_L " << g(a.lisn_dcr) << "\n"
      << "XLISNL " << RAIL << "_L MAINS MEASL LISN\n"
      << "XLISNN " << RTN << "_L 0 MEASN LISN\n\n"
      << "* ---- the board ---------------------------------------------------\n"
      << "XBOARD " << RAIL << " " << CELL << " " << SW << " " << RTN
      << " CHASSIS " << deck.manifest["subckt"].get<std::string>() << "\n"
      << "* the chassis IS the earth the LISNs measure against: this is the\n"
      << "* common-mode loop, and it closes through C_STRAY\n"
      << "VCH CHASSIS 0 DC 0\n\n"
      << "* ---- the switching cell (synchronous buck) ------------------------\n"
      << "MQH " << CELL << " GH " << SW << " " << a.model_name << "\n"
      << "MQL " << SW << " GL " << RTN << " " << a.model_name << "\n"
      << "RGH GHD GH " << g(a.rg) << "\n"
      << "RGL GLD GL " << g(a.rg) << "\n"
      << "* high side floats on the switch node, as a real bootstrap drive does\n"
      << "VGH GHD " << SW << " PULSE(0 " << g(a.vdrive) << " " << g(dead) << " "
      << g(tr) << " " << g(tr) << " " << g(ton - 2 * dead) << " " << g(T) << ")\n"
      << "VGL GLD " << RTN << " PULSE(" << g(a.vdrive) << " 0 0 " << g(tr) << " "
      << g(tr) << " " << g(ton) << " " << g(T) << ")\n\n"
      << "* ---- the load: a resistor, so the operating point is self-consistent\n"
      << "*   " << g(v_out, 4) << " V across " << g(r_load, 4) << " ohm = "
      << g(a.iload, 4) << " A at the requested duty\n"
      << "LOUT " << SW << " VOUT " << g(a.l_out_h) << "\n"
      << "COUT VOUT " << RTN << " " << g(c_out) << "\n"
      << "RLOAD VOUT " << RTN << " " << g(r_load) << "\n\n"
      << ".options reltol=1e-3 abstol=1e-9 vntol=1e-6 chgtol=1e-14 method=gear\n"
      << "* No .ic: the DC operating point starts with the low side on and the\n"
      << "* output at zero, which is where a real converter starts. Forcing the\n"
      << "* output instead drove the inductor — a short at DC — to -340 A in\n"
      << "* the operating point itself.\n";

    const double t_settle = a.settle_cycles * T;
    const double t_stop = t_settle + a.capture_cycles * T;
    const double t_max = std::min(tr / 10.0, T / 2000.0);
    d << ".tran " << g(t_max) << " " << g(t_stop) << " 0 " << g(t_max) << "\n"
      << ".end\n";

    if (!a.dump_deck.empty()) {
        std::ofstream fo(a.dump_deck);
        fo << d.str();
        std::cerr << "wrote the assembled deck to " << a.dump_deck << "\n";
    }
    if (!Kirchhoff::ngspice_in_process_available())
        throw std::runtime_error(
            "this build has no libngspice — configure with "
            "-DFARADAY_KIRCHHOFF_ROOT/-DFARADAY_KIRCHHOFF_BUILD");

    std::cerr << "running " << g(t_stop * 1e6, 4) << " us at "
              << g(t_max * 1e9, 3) << " ns max step ("
              << (int)a.settle_cycles << " settle + "
              << (int)a.capture_cycles << " capture cycles)...\n";
    auto r = Kirchhoff::run_ngspice_in_process(d.str(), 900.0);
    if (!r.success)
        throw std::runtime_error("the deck did not converge: " + r.error +
                                 " — a half-converged waveform makes a "
                                 "beautiful spectrum out of numerical noise, "
                                 "so nothing is written");

    // the receiver ports, by name, however ngspice spelled them
    // Vector naming varies with how ngspice was asked (bare node, v(node), a
    // plot prefix). Match on the node name with any wrapper stripped, and NAME
    // the vector that was used in the output — a silently mismatched vector is
    // how "vNeutral is exactly zero" happens without anything looking wrong.
    std::string used_l, used_n;
    auto vec = [&](const std::string& want, std::string& used)
        -> const std::vector<double>* {
        for (const auto& [k, v] : r.vectors) {
            std::string lk;
            for (char c : k) lk += (char)std::tolower((unsigned char)c);
            const size_t o = lk.find('(');
            const size_t c = lk.rfind(')');
            std::string node = (o != std::string::npos && c != std::string::npos && c > o)
                                   ? lk.substr(o + 1, c - o - 1) : lk;
            const size_t dot = node.rfind('.');
            if (dot != std::string::npos) node = node.substr(dot + 1);
            if (node == want) { used = k; return &v; }
        }
        return nullptr;
    };
    const auto* vl = vec("measl", used_l);
    const auto* vn = vec("measn", used_n);
    std::cerr << "vectors: " << r.vectors.size() << " back; using '" << used_l
              << "' and '" << used_n << "'\n";
    if (!vl || !vn)
        throw std::runtime_error(
            "the run produced no receiver-port voltages — the LISN did not "
            "connect, which means the deck is wrong rather than the board");

    // Steady state is a CLAIM, so check it: the capture window's mean must not
    // still be moving. Compare the two halves of the captured span.
    size_t i0 = 0;
    while (i0 < r.time.size() && r.time[i0] < t_settle) ++i0;
    if (i0 + 16 >= r.time.size())
        throw std::runtime_error("the run ended before the settle window");
    const size_t mid = i0 + (r.time.size() - i0) / 2;
    auto mean = [&](const std::vector<double>& v, size_t from, size_t to) {
        double s = 0;
        for (size_t i = from; i < to; ++i) s += v[i];
        return s / std::max<size_t>(1, to - from);
    };
    const double m1 = mean(*vl, i0, mid), m2 = mean(*vl, mid, vl->size());
    const double drift = std::abs(m2 - m1);

    nlohmann::json out{
        {"faraday", "0.2.0"},
        {"source", "simulated"},
        {"lisn", {{"name", lisn.name}, {"dcrOhm", a.lisn_dcr},
                  {"inductanceH", lisn.inductanceH},
                  {"couplingCapacitanceF", lisn.couplingCapacitanceF},
                  {"measuringImpedanceOhm", lisn.measuringImpedanceOhm}}},
        {"operatingPoint", {{"vinV", a.vin}, {"duty", a.duty},
                            {"dutyEffective", duty_eff},
                            {"vOutV", v_out}, {"rLoadOhm", r_load},
                            {"cOutF", c_out}, {"fSwHz", a.fsw},
                            {"iLoadA", a.iload}, {"rgOhm", a.rg},
                            {"cell", a.cell}}},
        {"modelCard", a.model_file},
        {"vectors", {{"line", used_l}, {"neutral", used_n}}},
        {"settleSeconds", t_settle},
        {"driftV", drift},
        {"parasitics", deck.manifest},
        {"commonModePathPresent", have_cm_path}};
    nlohmann::json t = nlohmann::json::array(), a_l = nlohmann::json::array(),
                   a_n = nlohmann::json::array();
    for (size_t i = i0; i < r.time.size(); ++i) {
        t.push_back(r.time[i]);
        a_l.push_back((*vl)[i]);
        a_n.push_back((*vn)[i]);
    }
    out["t"] = t;
    out["vLine"] = a_l;
    out["vNeutral"] = a_n;
    if (!have_cm_path)
        out["warnings"].push_back(
            "no C_STRAY in the parasitics (no chassis gap was stated), so the "
            "common-mode path is missing: the CM spectrum from this run is not "
            "a prediction of anything");
    if (drift > 0.05)
        out["warnings"].push_back(
            "the receiver port was still drifting by " + g(drift, 3) +
            " V across the capture window — increase --capture-cycles or the "
            "settle time before trusting the low-frequency end");

    std::ofstream fo(a.out);
    fo << out.dump();
    std::cout << "wrote " << a.out << " — " << t.size()
              << " samples of both line voltages, " << g(drift * 1e3, 3)
              << " mV drift across the window\n";
    for (const auto& w : out.value("warnings", nlohmann::json::array()))
        std::cout << "  warning: " << w.get<std::string>() << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "refused: " << e.what() << "\n";
    return 1;
}
