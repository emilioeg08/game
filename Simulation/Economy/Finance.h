#pragma once

#include "Engine/Core/Types.h"

// Credit and insurance (prompt §18, ADR-033). Two ledgers with explicit balance sheets: every credit that
// moves through them is booked on both sides, so their identities hold at any moment and can be checked
// (tests, inspectors). The balances of each borrower or depositor live with whoever owns them (a ship, a
// colony, a House); the ledgers keep the totals. No space dependency.
namespace gx {

// A rate learned from experience, shrunk towards a prior and with old experience fading away:
//   estimate = (observed + prior * priorExposure) / (exposure + priorExposure)
// For counts (losses per ship-hour) it is the posterior mean of a Gamma-Poisson model with a
// Gamma(prior * priorExposure, priorExposure) prior; for amounts (earnings per ship-hour) it is a
// credibility-weighted mean. With little exposure the prior dominates; with a lot, the evidence does.
struct ExperienceRate {
    f64 observed = 0.0; // decayed sum of what happened (losses, credits earned)
    f64 exposure = 0.0; // decayed exposure it happened over (hours)

    // Forgets `seconds` worth of experience: everything is halved every `halfLife` seconds.
    void decay(f64 seconds, f64 halfLife);
    void add(f64 value, f64 hours) {
        observed += value;
        exposure += hours;
    }
    [[nodiscard]] f64 estimate(f64 prior, f64 priorExposure) const;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("observed", observed);
        ar.io("exposure", exposure);
    }
};

// A bank: lends its cash (capital plus deposits) and earns the spread between what borrowers pay and what
// depositors get. Balance sheet: assets = cash + loans; liabilities = deposits; equity = assets -
// liabilities. Identity: equity() == capital + interestEarned - interestPaid - writtenOff. Cash may go
// negative when withdrawals exceed it (the bank then borrows outside the system and stops lending until it
// recovers).
struct BankLedger {
    i64 cash = 0;
    i64 loans = 0;    // principal owed by the borrowers
    i64 deposits = 0; // owed to the depositors
    i64 capital = 0;  // paid in when the bank opened
    i64 interestEarned = 0;
    i64 interestPaid = 0; // credited to deposits
    i64 writtenOff = 0;   // principal never recovered
    u64 loansGranted = 0;
    u64 defaults = 0;

    void open(i64 paidIn) {
        capital += paidIn;
        cash += paidIn;
    }
    [[nodiscard]] i64 equity() const { return cash + loans - deposits; }
    [[nodiscard]] bool balanced() const {
        return equity() == capital + interestEarned - interestPaid - writtenOff;
    }
    // Room to lend `amount` while keeping `reserveRatio` of the deposits in cash.
    [[nodiscard]] bool canLend(i64 amount, f64 reserveRatio) const;
    // What deposits can earn so that they never cost more than `passThrough` of what the loans earn: savings
    // nobody borrows earn nearly nothing.
    [[nodiscard]] f64 depositRate(f64 loanRate, f64 maxRate, f64 passThrough) const;

    void lend(i64 amount); // the borrower receives `amount` in credits
    void repay(i64 principal);
    void receiveInterest(i64 amount);
    void deposit(i64 amount);
    void withdraw(i64 amount);
    void creditInterest(i64 amount); // to the deposits: compounded, not paid out
    // A borrower's savings pay its own debt (both sides shrink, cash does not move).
    void offset(i64 amount);
    void writeOff(i64 principal);

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("cash", cash);
        ar.io("loans", loans);
        ar.io("deposits", deposits);
        ar.io("capital", capital);
        ar.io("interestEarned", interestEarned);
        ar.io("interestPaid", interestPaid);
        ar.io("writtenOff", writtenOff);
        ar.io("loansGranted", loansGranted);
        ar.io("defaults", defaults);
    }
};

// A mutual insurer: members pay premiums into a fund that pays their claims, total losses and repairs
// alike. The premium follows the fund's own experience (ExperienceRate of what claims cost per insured
// hour), so the price of risk moves with what actually happens. Identity: fund == capital + premiums -
// claimsPaid. A total loss larger than the fund is paid only in part; a repair the fund cannot pay is not
// covered.
struct MutualLedger {
    i64 fund = 0;
    i64 capital = 0;
    i64 premiums = 0;
    i64 claimsPaid = 0; // total losses and repairs
    i64 repairsPaid = 0;
    u64 claims = 0;           // total losses
    u64 claimsShort = 0;      // paid only in part: the fund ran dry
    ExperienceRate losses;    // total losses per insured hour (a count)
    ExperienceRate claimCost; // credits claimed per insured hour

    void open(i64 paidIn) {
        capital += paidIn;
        fund += paidIn;
    }
    [[nodiscard]] bool balanced() const { return fund == capital + premiums - claimsPaid; }
    // Premium per insured hour: expected claims (credits per insured hour, from experience) plus loading.
    [[nodiscard]] f64 premiumPerHour(f64 priorCost, f64 priorHours, f64 loading) const;

    // Premiums and exposure: `hours` of insured time.
    void collect(i64 premium, f64 hours);
    // A total loss: pays what the fund can of `amount` and returns it.
    i64 pay(i64 amount);
    // A repair: paid in full if the fund can, else not at all.
    bool cover(i64 amount);

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("fund", fund);
        ar.io("capital", capital);
        ar.io("premiums", premiums);
        ar.io("claimsPaid", claimsPaid);
        ar.io("repairsPaid", repairsPaid);
        ar.io("claims", claims);
        ar.io("claimsShort", claimsShort);
        ar.io("losses", losses);
        ar.io("claimCost", claimCost);
    }
};

} // namespace gx
