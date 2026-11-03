#pragma once
#include "app/smart_runner.h"
#include <QWidget>
#include <functional>

namespace ccg {
struct CandidateSnapshot {
    ccbot::SmartRunner::Report report;
    std::vector<ccbot::TrendEngine::Inspection> bots;
    bool connected = false;
};
// Read-only view: the provider takes a snapshot, never submits orders or changes settings.
QWidget* make_candidate_monitor(std::function<CandidateSnapshot()> provider, QWidget* parent = nullptr);
CandidateSnapshot read_candidate_checkpoint(const QString& path);
}
