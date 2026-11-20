#pragma once

static void test_close_retry_scenarios() {
    // Reproduce the cadence of the RAVE incident with synthetic prices, quantity, and timestamps.
    struct Venue : FakeClient {
        std::vector<std::string> posts, lookups;
        OrderOutcome response, lookup;
        bool lose_second_position_read = false;
        CloseVerification verify_close(const std::string& sym, const std::string& side, const std::string& stop) override {
            auto receipt = FakeClient::verify_close(sym, side, stop);
            if (lose_second_position_read && close_checks == 2) return {};
            return receipt;
        }
        OrderOutcome place_tagged_order(const std::string&, const std::string& side, double qty,
                                        bool reduce_only, const std::string& tag) override {
            check(side == "SELL" && reduce_only && qty > 0 && !tag.empty(), "closing POST has a unique lookup key");
            posts.push_back(tag);
            auto result = response;
            result.client_order_id = tag;
            return result;
        }
        OrderOutcome confirmed_order(const std::string&, const std::string& tag) override {
            lookups.push_back(tag);
            auto result = lookup;
            result.client_order_id = tag;
            return result;
        }
    };
    auto holding = [] {
        TrendBot b;
        b.cfg = mk_cfg("RAVEUSDT");
        b.cfg.rule.reverse = trend::ReverseMode::None;
        b.cfg.use_disaster_stop = true;
        b.st.pos = trend::Pos::Long;
        b.st.entry_price = 100;
        b.st.peak = 110;
        b.open_stop = 90; b.st.stop = 105;
        b.qty = 12;
        b.disaster_stop_id = "stop-A";
        b.disaster_stop_price = 89;
        return b;
    };
    advance(3);
    {
        auto venue = std::make_shared<Venue>();
        TrendEngine engine(venue, inline_host());
        std::vector<std::string> logs;
        std::vector<TrendTrade> trades;
        engine.restore_bot(holding());
        engine.set_log_cb([&](const auto& msg) { logs.push_back(msg); });
        engine.set_trade_cb([&](const auto& t) { trades.push_back(t); });
        check(bot_of(engine, "RAVEUSDT").ds_last_confirmed_id.empty(), "restored local stop is not an exchange confirmation");
        engine.reconcile_stop_orders(engine.capture_stop_orders(), {"stop-A"});
        advance(4);
        venue->response.error = "untrusted raw server message";
        venue->response.submit_diagnostic = "POST HTTP 503 / -1007；原单连续查无记录 3/3";
        venue->response.not_found = true;
        for (int i = 0; i < 13; ++i) {
            engine.tick("RAVEUSDT", 104);
            advance(3);
        }
        auto held = bot_of(engine, "RAVEUSDT");
        const std::set<std::string> distinct(venue->posts.begin(), venue->posts.end());
        check(venue->posts.size() == 13 && distinct.size() == 13 && held.qty == 12 &&
              held.disaster_stop_id == "stop-A" && venue->ds_count("cancel") == 0 && trades.empty(),
              "thirteen confirmed misses keep the hard stop and re-evaluate every tick");
        auto matching = [&](const std::string& text) {
            return std::count_if(logs.begin(), logs.end(), [&](const auto& line) { return line.find(text) != std::string::npos; });
        };
        check(matching("本仓平仓提交") == 1 && matching("HTTP 503") == 1 &&
              matching("untrusted raw") == 0, "only the first repeated failure is logged, with sanitized provenance");
        check(matching("保留原硬止损单 stop-A，登记触发价=89，最近收到该单交易所确认距今 4 秒") == 1,
              "close diagnostic names the existing stop, its price and the age of actual exchange evidence");
        venue->response = {true, "filled", "", 103.5, 12, false, {}};
        engine.tick("RAVEUSDT", 104);
        check(venue->posts.size() == 14 && trades.size() == 1 && trades.front().exit_price == 103.5 &&
              matching("本仓平仓提交 14 次") == 1 && matching("历时 39 秒") == 1 && venue->ds_count("cancel") == 1,
              "confirmed fourteenth close reports elapsed time, counts and cancels protection once");
    }
    {
        auto venue = std::make_shared<Venue>();
        TrendEngine engine(venue, inline_host());
        engine.restore_bot(holding());
        venue->response.uncertain = true;
        TrendBot durable;
        engine.set_stop_checkpoint("test", [&](const std::vector<TrendBot>& snapshot) {
            durable = snapshot.at(0); return true;
        });
        engine.tick("RAVEUSDT", 104);
        const auto key = venue->posts.at(0);
        check(durable.pending_close_key == key && !durable.pending_close_seen_on_exchange,
              "key saved before POST even without a profit ledger");
        TrendEngine resumed(venue, inline_host());
        resumed.restore_bot(durable);
        auto recheck = [&] { advance(16); resumed.maintain_paused_hard_stops(); };
        venue->lookup.uncertain = venue->lookup.not_found = true;
        venue->close_verification.position_known = true;
        venue->close_verification.remaining_qty = 12;
        recheck();
        check(venue->close_checks == 0 && bot_of(resumed, "RAVEUSDT").pending_close_key == key,
              "one missing close lookup after restart retains original key");
        venue->lookup.not_found = false; recheck(); // A transport failure breaks consecutive absences.
        venue->lookup.not_found = true; recheck();
        check(venue->close_checks == 0 && venue->posts.size() == 1, "mixed lookup failures cannot release pending close");
        venue->close_verification.position_known = false; recheck();
        check(!bot_of(resumed, "RAVEUSDT").pending_close_key.empty() && venue->ds_count("cancel") == 0,
              "missing position evidence keeps original order and protection");
        venue->close_verification.position_known = true; recheck();
        check(bot_of(resumed, "RAVEUSDT").pending_close_key.empty() && venue->posts.size() == 1 &&
              venue->lookups.size() == 5 && std::all_of(venue->lookups.begin(), venue->lookups.end(),
              [&](const auto& id) { return id == key; }), "repeated absence plus unchanged position releases only the original intent");
        resumed.tick("RAVEUSDT", 107);
        check(venue->posts.size() == 1, "recovery cannot replay a stale crossing");
        venue->response = {true, "partial", "", 104, 5, false, {}};
        resumed.tick("RAVEUSDT", 103);
        check(venue->posts.size() == 2 && venue->posts.back() != key && bot_of(resumed, "RAVEUSDT").qty == 7 &&
              bot_of(resumed, "RAVEUSDT").disaster_stop_id == "stop-A" && venue->ds_count("cancel") == 0,
              "new qualifying close only; partial fill preserves remaining position and protection");
    }
    {
        auto venue = std::make_shared<Venue>();
        TrendEngine engine(venue, inline_host());
        engine.restore_bot(holding());
        venue->response.uncertain = true;
        std::vector<ExecutionEvent> receipts;
        engine.set_execution_cb([&](ExecutionEvent& e) { if (e.completed) receipts.push_back(e); return true; });
        engine.tick("RAVEUSDT", 104);
        venue->lookup = {false, "known-order", "working", 0, 0, true, {}};
        advance(16); engine.maintain_paused_hard_stops();
        const auto saved = bot_of(engine, "RAVEUSDT");
        check(saved.pending_close_seen_on_exchange && receipts.size() == 2 && receipts.back().key == venue->posts.front() &&
              receipts.back().result.order_id == "known-order" && receipts.back().result.uncertain,
              "first positive exchange acknowledgment is checkpointed and recorded in the ledger");
        TrendEngine resumed(venue, inline_host());
        resumed.restore_bot(saved);
        venue->lookup = {}; venue->lookup.not_found = venue->lookup.uncertain = true;
        for (int i = 0; i < 2; ++i) { advance(16); resumed.maintain_paused_hard_stops(); }
        check(venue->close_checks == 0 && venue->posts.size() == 1 &&
              bot_of(resumed, "RAVEUSDT").pending_close_key == venue->posts.front(),
              "two later misses cannot release a positively identified order across restart");
        venue->lookup = {true, "known-order", "", 104, 12, false, {}};
        advance(16); resumed.maintain_paused_hard_stops();
        check(venue->posts.size() == 1 && bot_of(resumed, "RAVEUSDT").qty == 0 && venue->ds_count("cancel") == 1,
              "late full fill cancels hard stop only after confirmation without duplicate POST");
    }
    for (const double remaining : {7.0, 0.0}) {
        auto venue = std::make_shared<Venue>();
        TrendEngine engine(venue, inline_host());
        engine.restore_bot(holding());
        std::vector<TrendTrade> trades;
        engine.set_trade_cb([&](const auto& t) { trades.push_back(t); });
        venue->response.uncertain = true;
        engine.tick("RAVEUSDT", 104);
        venue->lookup.not_found = venue->lookup.uncertain = true;
        venue->close_verification.position_known = true;
        venue->close_verification.remaining_qty = remaining;
        if (remaining == 0) venue->close_verification.protective_fill = {true, "stop-fill", "", 88.5, 12, false, {}};
        for (int i = 0; i < 2; ++i) { advance(16); engine.maintain_paused_hard_stops(); }
        const auto verified = bot_of(engine, "RAVEUSDT");
        check(venue->posts.size() == 1 && verified.pending_close_key.empty() && verified.qty == remaining &&
              venue->ds_count("cancel") == 0, "external quantity changes never submit another close or cancel a live stop early");
        if (remaining > 0) {
            check(trades.empty() && verified.disaster_stop_id == "stop-A", "external partial quantity is not booked as a known market fill");
        } else {
            check(trades.size() == 1 && trades.front().exit_price == 88.5 && trades.front().exit_evidence.rule == "交易所硬止损",
                  "only independent protective fill evidence can book the external stop");
        }
    }
    {
        auto venue = std::make_shared<Venue>();
        TrendEngine engine(venue, inline_host()); engine.restore_bot(holding());
        int missing_receipts = 0;
        engine.set_execution_cb([&](ExecutionEvent& e) {
            if (e.completed && e.result.not_found && !e.result.uncertain) ++missing_receipts;
            return true;
        });
        venue->response.uncertain = true;
        engine.tick("RAVEUSDT", 104);
        venue->lookup.not_found = venue->lookup.uncertain = true;
        venue->close_verification.position_known = true;
        venue->lose_second_position_read = true;
        for (int i = 0; i < 2; ++i) { advance(16); engine.maintain_paused_hard_stops(); }
        auto held = bot_of(engine, "RAVEUSDT");
        check(venue->posts.size() == 1 && held.pending_close_key == venue->posts.front() && held.qty == 12 &&
              held.disaster_stop_id == "stop-A" && missing_receipts == 0,
              "ambiguous second flat-position read preserves original close key, ledger and stop");
        advance(16); engine.maintain_paused_hard_stops();
        held = bot_of(engine, "RAVEUSDT");
        check(venue->posts.size() == 1 && held.qty == 0 && held.pending_close_key.empty() && missing_receipts == 1,
              "two corroborating flat checks permit external close reconciliation");
    }
}
