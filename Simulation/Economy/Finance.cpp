#include "Simulation/Economy/Finance.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>

namespace gx {

void ExperienceRate::decay(f64 seconds, f64 halfLife) {
    if (seconds <= 0.0 || halfLife <= 0.0) {
        return;
    }
    const f64 keep = std::exp2(-seconds / halfLife);
    observed *= keep;
    exposure *= keep;
}

f64 ExperienceRate::estimate(f64 prior, f64 priorExposure) const {
    const f64 weight = exposure + priorExposure;
    return weight > 0.0 ? (observed + prior * priorExposure) / weight : prior;
}

bool BankLedger::canLend(i64 amount, f64 reserveRatio) const {
    return amount >= 0 &&
           static_cast<f64>(cash - amount) >= reserveRatio * static_cast<f64>(std::max<i64>(deposits, 0));
}

f64 BankLedger::depositRate(f64 loanRate, f64 maxRate, f64 passThrough) const {
    if (deposits <= 0) {
        return maxRate;
    }
    const f64 utilization = static_cast<f64>(std::max<i64>(loans, 0)) / static_cast<f64>(deposits);
    return std::min(maxRate, loanRate * utilization * passThrough);
}

void BankLedger::lend(i64 amount) {
    GX_ASSERT(amount >= 0);
    cash -= amount;
    loans += amount;
    loansGranted += amount > 0 ? 1 : 0;
}

void BankLedger::repay(i64 principal) {
    GX_ASSERT(principal >= 0 && principal <= loans);
    loans -= principal;
    cash += principal;
}

void BankLedger::receiveInterest(i64 amount) {
    GX_ASSERT(amount >= 0);
    cash += amount;
    interestEarned += amount;
}

void BankLedger::deposit(i64 amount) {
    GX_ASSERT(amount >= 0);
    cash += amount;
    deposits += amount;
}

void BankLedger::withdraw(i64 amount) {
    GX_ASSERT(amount >= 0 && amount <= deposits);
    cash -= amount;
    deposits -= amount;
}

void BankLedger::creditInterest(i64 amount) {
    GX_ASSERT(amount >= 0);
    deposits += amount;
    interestPaid += amount;
}

void BankLedger::offset(i64 amount) {
    GX_ASSERT(amount >= 0 && amount <= deposits && amount <= loans);
    deposits -= amount;
    loans -= amount;
}

void BankLedger::writeOff(i64 principal) {
    GX_ASSERT(principal >= 0 && principal <= loans);
    loans -= principal;
    writtenOff += principal;
    defaults += principal > 0 ? 1 : 0;
}

f64 MutualLedger::premiumPerHour(f64 priorCost, f64 priorHours, f64 loading) const {
    return claimCost.estimate(priorCost, priorHours) * (1.0 + loading);
}

void MutualLedger::collect(i64 premium, f64 hours) {
    GX_ASSERT(premium >= 0);
    fund += premium;
    premiums += premium;
    losses.add(0.0, hours);
    claimCost.add(0.0, hours);
}

i64 MutualLedger::pay(i64 amount) {
    GX_ASSERT(amount >= 0);
    const i64 paid = std::clamp<i64>(fund, 0, amount);
    fund -= paid;
    claimsPaid += paid;
    ++claims;
    claimsShort += paid < amount ? 1 : 0;
    losses.add(1.0, 0.0);
    claimCost.add(static_cast<f64>(amount), 0.0); // the risk is what was lost, whether or not it was paid
    return paid;
}

bool MutualLedger::cover(i64 amount) {
    GX_ASSERT(amount >= 0);
    claimCost.add(static_cast<f64>(amount), 0.0);
    if (fund < amount) {
        return false;
    }
    fund -= amount;
    claimsPaid += amount;
    repairsPaid += amount;
    return true;
}

} // namespace gx
