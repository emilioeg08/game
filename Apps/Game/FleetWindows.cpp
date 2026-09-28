// The player's company (ADR-037): the fleet window (its ships and their standing orders), the company's
// books and the station's yards, plus the mining controls of the ship the player flies. Everything the
// player decides here is a command; everything shown comes from the snapshot.
#include "Apps/Game/GameApp.h"

#include "Engine/Text/Localization.h"
#include "Game/Sandbox/Content.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <utility>

namespace gx {
namespace {

ImVec4 color(u8 r, u8 g, u8 b, u8 a = 255) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

const char* orderName(FleetOrder order) {
    switch (order) {
    case FleetOrder::Hold:
        return tr("Mantener posición");
    case FleetOrder::Dock:
        return tr("Atracar");
    case FleetOrder::Mine:
        return tr("Minar");
    case FleetOrder::Trade:
        return tr("Comerciar");
    case FleetOrder::Escort:
        return tr("Escoltar");
    case FleetOrder::Count:
        break;
    }
    return "?";
}

const char* taskName(FleetTask task) {
    switch (task) {
    case FleetTask::Idle:
        return tr("en espera");
    case FleetTask::Travelling:
        return tr("en ruta");
    case FleetTask::Mining:
        return tr("minando");
    case FleetTask::Fleeing:
        return tr("huyendo / en reparación");
    case FleetTask::Escorting:
        return tr("escoltando");
    case FleetTask::Engaging:
        return tr("combatiendo");
    case FleetTask::Count:
        break;
    }
    return "?";
}

// "12 345 cr" style: thousands grouped, so books read at a glance.
std::string credits(i64 value) {
    std::string digits = std::to_string(value < 0 ? -value : value);
    for (auto i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<usize>(i), " ");
    }
    return (value < 0 ? "-" : "") + digits + " cr";
}

} // namespace

void GameApp::drawFleetWindow() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos({display.x * 0.5f - 280.0f, 120.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({560.0f, display.y * 0.6f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Flota y empresa (L)"), &m_showFleet)) {
        ImGui::End();
        return;
    }
    const CompanyView& company = m_snapshot.company;
    ImGui::Text(tr("Cuenta: %s   ·   Deuda: %s   ·   Patrimonio: %s"), credits(company.account).c_str(),
                credits(company.debt).c_str(), credits(company.worth).c_str());
    if (company.overdrawn) {
        ImGui::TextColored(color(255, 110, 100), "%s",
                           tr("Cuenta en descubierto: si no se cubre, el banco venderá una nave."));
    }
    const std::string request = std::exchange(m_fleetTabRequest, std::string());
    const auto flags = [&](const char* tab) {
        return request == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabBar("company")) {
        if (ImGui::BeginTabItem(tr("Flota"), nullptr, flags("fleet"))) {
            drawFleetList();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Cuentas"), nullptr, flags("books"))) {
            drawCompanyBooks();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Astillero"), nullptr, flags("yards"))) {
            drawShipyard();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void GameApp::drawFleetList() {
    if (m_snapshot.fleet.empty()) {
        ImGui::TextWrapped("%s",
                           tr("Tu compañía no tiene más naves que la tuya. Compra una en el astillero de "
                              "cualquier estación: un Minero para explotar los campos de asteroides, un "
                              "Carguero que comercie por su cuenta o una Escolta que proteja a las demás."));
        return;
    }
    if (ImGui::BeginTable("fleet", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                          {0.0f, 180.0f})) {
        ImGui::TableSetupColumn(tr("Nave"));
        ImGui::TableSetupColumn(tr("Orden"));
        ImGui::TableSetupColumn(tr("Estado"));
        ImGui::TableSetupColumn(tr("Carga"));
        ImGui::TableSetupColumn(tr("Casco"));
        ImGui::TableSetupColumn(tr("Resultado"));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (const FleetShipView& ship : m_snapshot.fleet) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label =
                std::format("{} ({})###fleet{}", ship.name, tr(content::kShipClasses[ship.shipClass].name),
                            ship.id.index);
            if (ImGui::Selectable(label.c_str(), m_selected == ship.id,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                m_selected = ship.id;
                m_selectedContact = 0;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(orderName(ship.order));
            ImGui::TableNextColumn();
            if (!ship.powered) {
                ImGui::TextColored(color(255, 90, 80), "%s", tr("sin energía"));
            } else {
                ImGui::TextUnformatted(taskName(ship.task));
            }
            ImGui::TableNextColumn();
            if (ship.cargoCapacity > 0) {
                ImGui::Text("%u / %u t", ship.cargoUsed, ship.cargoCapacity);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(ship.structure < 0.5 ? color(255, 120, 100) : color(170, 200, 170), "%.0f%%",
                               ship.structure * 100.0);
            ImGui::TableNextColumn();
            const i64 net = ship.income - ship.expenses;
            ImGui::TextColored(net >= 0 ? color(120, 230, 140) : color(255, 130, 110), "%s",
                               credits(net).c_str());
        }
        ImGui::EndTable();
    }
    const FleetShipView* selected = m_snapshot.findFleetShip(m_selected);
    ImGui::Separator();
    if (selected == nullptr) {
        ImGui::TextDisabled("%s", tr("Selecciona una nave para darle órdenes."));
        return;
    }
    drawFleetOrders(*selected);
}

void GameApp::drawFleetOrders(const FleetShipView& ship) {
    ImGui::PushID(static_cast<int>(ship.id.index));
    ImGui::Text("%.*s  ·  %s", static_cast<int>(ship.name.size()), ship.name.data(),
                tr(content::kShipClasses[ship.shipClass].name));
    ImGui::TextDisabled(tr("Orden: %s  ·  %s"), orderName(ship.order), taskName(ship.task));
    if (ship.docked.isValid()) {
        ImGui::TextDisabled(tr("Atracada en %s"), nameOf(ship.docked).c_str());
    }
    ImGui::TextDisabled(tr("Ingresos %s  ·  gastos %s"), credits(ship.income).c_str(),
                        credits(ship.expenses).c_str());

    // The editor starts from the ship's current order whenever another ship is selected.
    if (m_orderShip != ship.id) {
        m_orderShip = ship.id;
        m_orderKind = ship.order;
        m_orderSite = ship.site;
        m_orderMarket = ship.market;
    }
    if (ImGui::BeginCombo(tr("Orden"), orderName(m_orderKind))) {
        for (const FleetOrder order :
             {FleetOrder::Hold, FleetOrder::Dock, FleetOrder::Mine, FleetOrder::Trade, FleetOrder::Escort}) {
            const bool possible = (order != FleetOrder::Mine || ship.canMine) &&
                                  (order != FleetOrder::Trade || ship.cargoCapacity > 0);
            if (ImGui::Selectable(orderName(order), m_orderKind == order,
                                  possible ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled)) {
                m_orderKind = order;
                m_orderSite = {};
                m_orderMarket = {};
            }
        }
        ImGui::EndCombo();
    }

    // Where: a port, a field and where to sell, or the ship to guard.
    const auto combo = [&](const char* label, EntityId& value, const std::vector<EntityId>& options,
                           const char* none, auto describe) {
        const std::string preview =
            value.isValid() ? describe(value) : std::string(none != nullptr ? none : "");
        if (ImGui::BeginCombo(label, preview.c_str())) {
            if (none != nullptr && ImGui::Selectable(none, !value.isValid())) {
                value = {};
            }
            for (const EntityId option : options) {
                if (ImGui::Selectable(describe(option).c_str(), value == option)) {
                    value = option;
                }
            }
            ImGui::EndCombo();
        }
    };
    const auto named = [&](EntityId entity) { return nameOf(entity); };
    const auto fieldLabel = [&](EntityId field) {
        const DepositView* deposit = m_snapshot.findDeposit(field);
        if (deposit == nullptr) {
            return nameOf(field);
        }
        return std::format("{}  ·  {} {:.0f}%", nameOf(field),
                           tr(std::string_view(sandbox().economy().goods()[deposit->good].name)),
                           deposit->size > 0.0 ? deposit->reserve / deposit->size * 100.0 : 0.0);
    };
    bool ready = true;
    switch (m_orderKind) {
    case FleetOrder::Dock:
        combo(tr("Puerto"), m_orderSite, sandbox().ports(), nullptr, named);
        ready = m_orderSite.isValid();
        break;
    case FleetOrder::Mine:
        combo(tr("Campo"), m_orderSite, sandbox().fields(), nullptr, fieldLabel);
        combo(tr("Vender en"), m_orderMarket, sandbox().ports(), tr("donde mejor paguen"), named);
        ready = m_orderSite.isValid();
        break;
    case FleetOrder::Escort: {
        std::vector<EntityId> wards;
        for (const FleetShipView& other : m_snapshot.fleet) {
            if (other.id != ship.id) {
                wards.push_back(other.id);
            }
        }
        combo(tr("Escoltar a"), m_orderSite, wards, tr("tu nave"), named);
        if (!ship.armed) {
            ImGui::TextDisabled("%s", tr("Sin armas: solo la acompaña."));
        }
        break;
    }
    default:
        break;
    }
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button(tr("Dar la orden"))) {
        simulation().submitCommand(FleetOrderCommand{ship.id, m_orderKind, m_orderSite, m_orderMarket});
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (m_snapshot.company.finance) {
        bool insured = ship.insured;
        if (ImGui::Checkbox(tr("Asegurada"), &insured)) {
            simulation().submitCommand(InsureCommand{ship.id, insured});
        }
        ImGui::SameLine();
        ImGui::TextDisabled(tr("prima %.0f cr/h"), m_snapshot.company.premiumPerHauler *
                                                       static_cast<f64>(ship.hullValue) /
                                                       static_cast<f64>(content::kHaulerHullPrice));
    }
    if (ImGui::Button(tr("Tomar el mando"))) {
        simulation().submitCommand(FlagshipCommand{ship.id});
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Seguir en el mapa"))) {
        m_map.camera().follow = ship.id;
    }
    ImGui::SameLine();
    const BodyView* dockedBody = m_snapshot.findBody(ship.docked);
    const bool atYards = dockedBody != nullptr && dockedBody->kind == BodyKind::Station;
    ImGui::BeginDisabled(!atYards);
    const std::string sell =
        trf("Vender ({})", credits(std::llround(content::kShipResale * static_cast<f64>(ship.hullValue))));
    if (ImGui::Button(sell.c_str())) {
        simulation().submitCommand(SellShipCommand{ship.id});
    }
    ImGui::EndDisabled();
    if (!atYards) {
        ImGui::TextDisabled("%s", tr("Se vende en el astillero de una estación, atracada."));
    }
    ImGui::PopID();
}

void GameApp::drawCompanyBooks() {
    const CompanyView& company = m_snapshot.company;
    const CompanyTotals& t = company.totals;
    if (ImGui::BeginTable("balance", 2, ImGuiTableFlags_SizingStretchProp)) {
        const auto row = [](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value.c_str());
        };
        row(tr("Cuenta"), credits(company.account));
        row(tr("Cascos (valor de compra)"), credits(company.fleetValue));
        row(tr("Carga a bordo (precio base)"), credits(company.cargoValue));
        row(tr("Deuda con el banco"), credits(-company.debt));
        row(tr("Patrimonio neto"), credits(company.worth));
        ImGui::EndTable();
    }
    if (company.finance) {
        ImGui::Separator();
        ImGui::Text(tr("El banco presta hasta el %.0f %% del valor de tus cascos: %s (disponible %s)."),
                    (1.0 - content::kMinDownPayment) * 100.0, credits(company.creditLimit).c_str(),
                    credits(std::max<i64>(0, company.creditLimit - company.debt)).c_str());
        ImGui::TextDisabled(tr("Interés %.0f %%/h  ·  amortización %s/h  ·  plazo %.0f h"),
                            content::kLoanRatePerHour * 100.0, credits(company.instalment * 60).c_str(),
                            content::kLoanTermHours);
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputScalar("##borrow", ImGuiDataType_S64, &m_borrowAmount, nullptr, nullptr, "%lld");
        ImGui::SameLine();
        if (ImGui::Button(tr("Pedir prestado")) && m_borrowAmount > 0) {
            simulation().submitCommand(LoanCommand{m_borrowAmount, 0});
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputScalar("##repay", ImGuiDataType_S64, &m_repayAmount, nullptr, nullptr, "%lld");
        ImGui::SameLine();
        ImGui::BeginDisabled(company.debt <= 0);
        if (ImGui::Button(tr("Devolver")) && m_repayAmount > 0) {
            simulation().submitCommand(LoanCommand{0, m_repayAmount});
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            tr("Seguro: %.0f cr/h por un casco de Carguero, según tu historial de siniestros."),
            company.premiumPerHauler);
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("Desde el comienzo de la partida"));
    if (ImGui::BeginTable("flows", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        const auto pair = [](const char* inLabel, i64 in, const char* outLabel, i64 out) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(inLabel);
            ImGui::TableNextColumn();
            ImGui::TextColored(color(120, 230, 140), "%s", credits(in).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(outLabel);
            ImGui::TableNextColumn();
            ImGui::TextColored(color(255, 150, 120), "%s", credits(-out).c_str());
        };
        pair(tr("Ventas"), t.sales, tr("Compras"), t.purchases);
        pair(tr("Contratos"), t.contracts, tr("Impuestos"), t.taxes);
        pair(tr("Recompensas"), t.bounties, tr("Salarios"), t.wages);
        pair(tr("Indemnizaciones"), t.claims, tr("Primas"), t.premiums);
        pair(tr("Naves vendidas"), t.shipsSold, tr("Reparaciones"), t.repairs);
        pair(tr("Préstamos"), t.borrowed, tr("Intereses"), t.interest);
        pair("", 0, tr("Devoluciones"), t.repaid);
        pair("", 0, tr("Naves compradas"), t.shipsBought);
        ImGui::EndTable();
    }
    ImGui::TextDisabled(
        tr("Minado: %llu t  ·  naves perdidas: %llu  ·  reparaciones cubiertas por el seguro: %s"),
        static_cast<unsigned long long>(t.tonnesMined), static_cast<unsigned long long>(t.shipsLost),
        credits(t.repairsInsured).c_str());
}

void GameApp::drawShipyard() {
    const BodyView* station = m_snapshot.findBody(m_snapshot.dockedPort);
    if (station == nullptr || station->kind != BodyKind::Station) {
        ImGui::TextWrapped("%s",
                           tr("Los astilleros están en las estaciones: atraca en una para comprar naves. "
                              "Las naves nuevas se entregan allí."));
        return;
    }
    const CompanyView& company = m_snapshot.company;
    ImGui::Text(tr("Astillero de %s"), nameOf(station->id).c_str());
    if (company.finance) {
        ImGui::Checkbox(tr("Asegurar la nave (obligatorio si la financia el banco)"), &m_insureNew);
    }
    for (const u32 shipClass : content::kShipsForSale) {
        ImGui::PushID(static_cast<int>(shipClass));
        const content::ShipClassDef& def = content::kShipClasses[shipClass];
        const i64 price = content::kShipPrices[shipClass];
        ImGui::Separator();
        ImGui::Text("%s  ·  %s", tr(def.name), credits(price).c_str());
        std::string capabilities = content::kCargoCapacity[shipClass] > 0
                                       ? trf("bodega {} t", content::kCargoCapacity[shipClass])
                                       : "";
        u32 lasers = 0;
        u32 weapons = 0;
        for (const content::ModuleDef& module : content::shipModules(shipClass)) {
            lasers += module.type == ModuleType::Mining ? 1 : 0;
            weapons += module.type == ModuleType::Weapon ? 1 : 0;
        }
        const auto add = [&](std::string part) {
            capabilities += (capabilities.empty() ? "" : "  ·  ") + part;
        };
        if (lasers > 0) {
            add(trf("{} láseres de minería ({:.0f} t/h)", lasers, content::kMiningRatePerModule * lasers));
        }
        if (weapons > 0) {
            add(trf("{} armas", weapons));
        }
        add(trf("1 UA en {:.0f} min de hiperespacio", 1.495978707e11 / def.hyperspaceSpeed / 60.0));
        ImGui::TextDisabled("%s", capabilities.c_str());

        // Financing: the least the bank accepts (its loan-to-value on the whole fleet) up to the full price.
        const auto room = static_cast<i64>((1.0 - content::kMinDownPayment) *
                                           static_cast<f64>(company.fleetValue + price)) -
                          company.debt;
        const i64 minimum = company.finance ? std::clamp<i64>(price - room, 0, price) : price;
        int& percent = m_downPercent[shipClass];
        const int minPercent =
            static_cast<int>(std::ceil(100.0 * static_cast<f64>(minimum) / static_cast<f64>(price)));
        percent = std::clamp(percent == 0 ? std::max(minPercent, 25) : percent, minPercent, 100);
        if (company.finance && minPercent < 100) {
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderInt(tr("entrada"), &percent, minPercent, 100, "%d %%");
        }
        const i64 down = percent >= 100 ? price : std::max(minimum, price * percent / 100);
        const i64 loan = price - down;
        const f64 wages = static_cast<f64>(content::kShipWagesPerMinute[shipClass]) * 60.0;
        const bool insured = company.finance && (m_insureNew || loan > 0);
        const f64 premium = insured ? company.premiumPerHauler * static_cast<f64>(price) /
                                          static_cast<f64>(content::kHaulerHullPrice)
                                    : 0.0;
        const f64 service = static_cast<f64>(loan) / content::kLoanTermHours +
                            static_cast<f64>(loan) * content::kLoanRatePerHour;
        ImGui::Text(tr("Entrada %s  ·  crédito %s"), credits(down).c_str(), credits(loan).c_str());
        ImGui::TextDisabled(tr("Costes: tripulación %.0f cr/h, seguro %.0f cr/h, deuda %.0f cr/h (%.0f h)"),
                            wages, premium, service, content::kLoanTermHours);
        const bool affordable = down <= company.account;
        ImGui::BeginDisabled(!affordable || minimum > price);
        if (ImGui::Button(tr("Comprar"))) {
            simulation().submitCommand(BuyShipCommand{sandbox().playerShip(), shipClass, down, insured});
        }
        ImGui::EndDisabled();
        if (!affordable) {
            ImGui::SameLine();
            ImGui::TextColored(color(255, 150, 110), "%s", tr("no tienes créditos para la entrada"));
        }
        ImGui::PopID();
    }
}

void GameApp::drawMiningSection(const ShipView& ship) {
    if (!m_snapshot.playerCanMine) {
        return;
    }
    ImGui::Separator();
    ImGui::Text(tr("Láseres de minería: %.0f t/h"), m_snapshot.playerMiningRate);
    if (m_snapshot.playerMining) {
        const DepositView* deposit = m_snapshot.findDeposit(m_snapshot.playerMiningField);
        ImGui::TextColored(color(230, 200, 120), tr("Minando %s"),
                           nameOf(m_snapshot.playerMiningField).c_str());
        if (deposit != nullptr) {
            ImGui::SameLine();
            ImGui::TextDisabled(tr("(quedan %.0f t)"), deposit->reserve);
        }
        if (ImGui::Button(tr("Detener la minería"))) {
            simulation().submitCommand(MineCommand{ship.id, m_snapshot.playerMiningField, false});
        }
        return;
    }
    // The nearest field, and whether the ship is among its rocks.
    const BodyView* nearest = nullptr;
    f64 distance = 0.0;
    for (const BodyView& body : m_snapshot.bodies) {
        const f64 d = length(body.position - ship.position);
        if (isAsteroidField(body.kind) && (nearest == nullptr || d < distance)) {
            nearest = &body;
            distance = d;
        }
    }
    if (nearest == nullptr) {
        return;
    }
    if (distance <= content::kMiningRange) {
        const std::string label = trf("Minar {}", localizedName(nearest->name));
        if (ImGui::Button(label.c_str())) {
            simulation().submitCommand(MineCommand{ship.id, nearest->id, true});
        }
    } else {
        ImGui::TextDisabled(tr("Campo más cercano: %s a %s"), localizedName(nearest->name).c_str(),
                            formatDistance(distance).c_str());
        if (ImGui::SmallButton(tr("Ir al campo"))) {
            submitPilot(FlightMode::Approach, nearest->id);
        }
    }
}

void GameApp::drawFieldSelection(EntityId field) {
    const DepositView* deposit = m_snapshot.findDeposit(field);
    if (deposit == nullptr) {
        return;
    }
    const std::string_view good = tr(std::string_view(sandbox().economy().goods()[deposit->good].name));
    ImGui::Text(tr("Recurso: %.*s"), static_cast<int>(good.size()), good.data());
    ImGui::ProgressBar(static_cast<float>(deposit->size > 0.0 ? deposit->reserve / deposit->size : 0.0),
                       {-FLT_MIN, 0.0f},
                       std::format("{:.0f} / {:.0f} t", deposit->reserve, deposit->size).c_str());
    ImGui::TextDisabled("%s", tr("Se recupera con el tiempo: rocas nuevas entran en alcance."));
    ImGui::TextDisabled(tr("Para minar: un Minero a menos de %s del centro, con la velocidad del campo."),
                        formatDistance(content::kMiningRange).c_str());
    bool any = false;
    for (const FleetShipView& ship : m_snapshot.fleet) {
        if (!ship.canMine) {
            continue;
        }
        if (!any) {
            ImGui::Separator();
            ImGui::TextDisabled("%s", tr("Ordenar minar aquí:"));
            any = true;
        }
        ImGui::PushID(static_cast<int>(ship.id.index));
        const std::string label = std::format("{}###mine", ship.name);
        if (ImGui::SmallButton(label.c_str())) {
            simulation().submitCommand(FleetOrderCommand{ship.id, FleetOrder::Mine, field, {}});
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s",
                            ship.order == FleetOrder::Mine && ship.site == field ? tr("(ya mina aquí)") : "");
        ImGui::PopID();
    }
}

} // namespace gx
