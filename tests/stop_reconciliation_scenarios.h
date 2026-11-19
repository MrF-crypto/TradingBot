#pragma once

#include <future>

static void test_stop_reconciliation_scenarios() {
    auto config = [](const std::string& sym, bool protected_order = true) {
        auto c = mk_cfg(sym);
        c.rule.reverse = trend::ReverseMode::None;
        c.use_disaster_stop = protected_order;
        return c;
    };
    auto signal = [&](TrendEngine& e, const std::string& sym, bool protected_order = true) {
        auto id = e.add_bot(config(sym, protected_order));
        feed(e, id, 2, 110, 90);
        return id;
    };
    auto openings = [](const FakeClient& cli) {
        return std::count_if(cli.calls.begin(), cli.calls.end(), [](const auto& c) { return !c.reduce_only; });
    };
    auto trip = [&](TrendEngine& e, FakeClient& cli) {
        cli.fill_price = 110;
        cli.ds_place_fails = true;
        cli.ds_fail_retryable = false;
        signal(e, "AAAUSDT"); e.tick("AAAUSDT", 110);
        signal(e, "BBBUSDT"); e.tick("BBBUSDT", 110);
        cli.ds_place_fails = false;
    };

    {
        auto cli = std::make_shared<FakeClient>();
        cli->fill_price = 110;
        TrendEngine e(cli, inline_host());
        auto id = signal(e, "TESTUSDT");
        const auto earlier = e.capture_stop_orders();
        e.tick("TESTUSDT", 110);
        const auto initial = bot_of(e, "TESTUSDT");
        check(e.reconcile_stop_orders(earlier, {}).empty(), "快照发起后才挂的止损不能被报消失");
        e.tick("TESTUSDT", 111);
        check(cli->ds_count("place") == 1 && bot_of(e, "TESTUSDT").disaster_stop_id == initial.disaster_stop_id,
              "旧快照回来仍保留原止损单号且不会重挂");
        const auto first = e.capture_stop_orders();
        check(e.reconcile_stop_orders(first, {}).size() == 1, "新鲜挂单列表仍能识别真实撤单");
        e.tick("TESTUSDT", 112);
        const auto replacement = bot_of(e, "TESTUSDT");
        check(replacement.disaster_stop_id != initial.disaster_stop_id && cli->ds_count("place") == 2 &&
              replacement.disaster_stop_price == initial.disaster_stop_price &&
              e.reconcile_stop_orders(first, {}).empty(), "迟到的旧单查询不能移除补挂的新单");

        cli->ds_cancel_ok = false;
        e.close_bot(id);
        const auto flat = bot_of(e, "TESTUSDT");
        check(flat.qty == 0 && flat.ds_cancel_id == replacement.disaster_stop_id &&
              flat.disaster_stop_id.empty() && !e.remove_if_flat(id), "撤单回执未知仍保留旧单号及恢复记录");
        e.maintain_paused_hard_stops();
        check(cli->ds_count("cancel") == 1, "同一撤单确认遵循重试间隔");
        signal(e, "NEXTUSDT"); e.tick("NEXTUSDT", 110);
        check(bot_of(e, "NEXTUSDT").qty > 0, "其他品种继续正常开仓并挂保护单");
        const auto active = bot_of(e, "NEXTUSDT").disaster_stop_id;
        const auto retiring = e.capture_stop_orders();
        e.reconcile_stop_orders(retiring, {replacement.disaster_stop_id, active});
        check(bot_of(e, "TESTUSDT").ds_cancel_id == replacement.disaster_stop_id,
              "交易所仍有这张旧单时不能解除撤单待核");
        e.reconcile_stop_orders(retiring, {active});
        check(bot_of(e, "TESTUSDT").ds_cancel_id.empty(), "完整的新快照可确认旧单已撤");
        cli->ds_cancel_ok = true;
        TrendEngine restarted(cli, inline_host());
        restarted.restore_bot(flat);
        restarted.restore_bot(bot_of(e, "NEXTUSDT"));
        restarted.maintain_paused_hard_stops();
        check(bot_of(restarted, "TESTUSDT").ds_cancel_id.empty() && cli->stop_log.back().id == replacement.disaster_stop_id &&
              bot_of(restarted, "NEXTUSDT").disaster_stop_id == active, "重启后只按旧单号补撤，已有持仓保护不动");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        cli->fill_price = 110; cli->ds_cancel_ok = false;
        TrendEngine e(cli, inline_host());
        auto id = signal(e, "SLOWUSDT");
        cli->during_stop = [&] {
            e.close_bot(id);
            e.tick("SLOWUSDT", 110);
            check(openings(*cli) == 1, "旧止损正在挂出时平仓，同币不能抢先再开");
        };
        e.tick("SLOWUSDT", 110);
        auto retired = bot_of(e, "SLOWUSDT");
        check(retired.qty == 0 && retired.ds_cancel_id == "DS1" && retired.disaster_stop_id.empty(),
              "平仓后才到的止损回执归入旧仓撤单，保留未知撤单记录");
        e.tick("SLOWUSDT", 111);
        check(openings(*cli) == 1, "旧单仍可能触发时同一币不重新开仓");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        cli->fill_price = 110;
        TrendEngine e(cli, inline_host());
        signal(e, "KEEPUSDT"); e.tick("KEEPUSDT", 110);
        trip(e, *cli);
        auto before = openings(*cli);
        signal(e, "NOHARDUSDT", false); signal(e, "DDDUSDT"); signal(e, "EEEUSDT");
        e.tick("DDDUSDT", 110);
        check(openings(*cli) == before, "熔断冷却中不开试探");
        advance(601);
        for (const auto& b : e.get_bots()) feed(e, b.bot_id, 2, 110, 90);
        e.tick("NOHARDUSDT", 110);
        check(openings(*cli) == before, "关闭硬止损的候选不能占用试探");
        e.reconcile_stop_orders(e.capture_stop_orders(), {});
        cli->during_order = [&] {
            e.tick("KEEPUSDT", 111);
            check(!bot_of(e, "KEEPUSDT").disaster_stop_id.empty(), "试探期间已有持仓仍能补挂保护单");
            e.tick("EEEUSDT", 110);
            check(openings(*cli) == before + 1, "并行信号和其他持仓补挂成功均不能释放试探名额");
            cli->during_stop = [&] {
                e.tick("EEEUSDT", 110);
                check(openings(*cli) == before + 1, "试探已成交但止损回执未到仍只放行一笔");
            };
        };
        e.tick("DDDUSDT", 110);
        e.tick("EEEUSDT", 110);
        check(openings(*cli) == before + 2 && !bot_of(e, "DDDUSDT").disaster_stop_id.empty() &&
              !bot_of(e, "DDDUSDT").ds_probe, "试探确认成交并已保护才恢复常规开仓");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine e(cli, inline_host());
        std::vector<TrendBot> saved;
        auto save = [&](const std::vector<TrendBot>& bots) { saved = bots; return true; };
        e.set_stop_checkpoint("solo", save);
        trip(e, *cli);
        for (const auto& b : e.get_bots()) e.remove_bot(b.bot_id);
        check(e.get_bots().empty() && e.save_checkpoint(save) && saved.size() == 1 &&
              saved.front().ds_account_only && saved.front().ds_probe_account == "solo",
              "全部品种移除后仅保存账户冷却，监控列表为空");
        TrendEngine restart(cli, inline_host());
        restart.set_stop_checkpoint("solo", save);
        restart.restore_bot(saved.front());
        check(restart.get_bots().empty(), "恢复账户冷却不创建机器人或占用品种名额");
        signal(restart, "AAAUSDT");
        const auto before = openings(*cli);
        restart.tick("AAAUSDT", 110);
        check(openings(*cli) == before, "品种全部删除后重启，新增候选仍遵守熔断冷却");
        advance(601);
        feed(restart, bot_of(restart, "AAAUSDT").bot_id, 2, 110, 90);
        restart.tick("AAAUSDT", 110);
        check(openings(*cli) == before + 1 && !bot_of(restart, "AAAUSDT").disaster_stop_id.empty(),
              "恢复冷却到期后才能按原流程试探并挂硬止损");
    }
    {
        struct ObservedPost : FakeClient {
            std::string tag;
            OrderOutcome place_tagged_order(const std::string& symbol, const std::string& side,
                                            double qty, bool reduce, const std::string& client_id) override {
                tag = client_id;
                auto order = place_market_order(symbol, side, qty, reduce);
                if (!tag.empty()) order.client_order_id = tag;
                return order;
            }
        };
        auto cli = std::make_shared<ObservedPost>();
        TrendEngine e(cli, inline_host());
        std::vector<TrendBot> durable;
        bool disk_ok = true;
        e.set_stop_checkpoint("account-A", [&](const std::vector<TrendBot>& bots) {
            if (disk_ok) durable = bots;
            return disk_ok;
        });
        trip(e, *cli);
        check(!durable.empty() && durable.front().ds_probe_after_ms > 0 &&
              durable.front().ds_probe_account == "account-A", "账户刚熔断即持久化，无需等待定时保存");
        advance(601);
        signal(e, "POSTUSDT");
        const auto before = openings(*cli);
        disk_ok = false;
        e.tick("POSTUSDT", 110);
        check(openings(*cli) == before && !bot_of(e, "POSTUSDT").ds_probe,
              "断点不能写入时不会发送试探，也不留下虚假的在途订单");
        disk_ok = true;
        cli->during_order = [&] {
            // POST has reached the exchange. Kill/restart here, before its receipt or the normal periodic save.
            TrendEngine restart(cli, inline_host());
            restart.set_stop_checkpoint("account-A", [](const auto&) { return true; });
            for (const auto& b : durable) restart.restore_bot(b);
            const auto owner = bot_of(restart, "POSTUSDT");
            check(owner.ds_probe && owner.open_uncertain && !owner.uncertain_client_id.empty() &&
                  owner.uncertain_client_id == cli->tag && cli->tag.size() <= 36 &&
                  owner.uncertain_stop > 0 && owner.reserved_dir == trend::Pos::Long,
                  "POST 后立即断电也已持久化精确订单号、方向和止损，可按原订单核对");
            signal(restart, "OTHERUSDT"); restart.tick("OTHERUSDT", 110);
            check(openings(*cli) == before + 1, "尚无常规定时断点时重启亦不重复发送试探");
            TrendEngine other(cli, inline_host());
            other.set_stop_checkpoint("account-B", [](const auto&) { return true; });
            for (const auto& b : durable) other.restore_bot(b);
            check(bot_of(other, "POSTUSDT").bot_id.empty(), "另一账户不能认领或保护本账户的试探委托");
        };
        e.tick("POSTUSDT", 110);
        check(openings(*cli) == before + 1 && !durable.empty() && !durable.front().ds_probe_after_ms &&
              !bot_of(e, "POSTUSDT").disaster_stop_id.empty(), "写入恢复后仅发一笔，确认保护后同步解除熔断");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine e(cli, inline_host());
        std::promise<void> writing, resume;
        auto release = resume.get_future();
        auto writer = std::async(std::launch::async, [&] {
            return e.save_checkpoint([&](const auto&) {
                writing.set_value();
                release.wait();
                return true;
            });
        });
        writing.get_future().wait();
        auto tick = std::async(std::launch::async, [&] {
            auto id = signal(e, "LIVEUSDT");
            e.tick("LIVEUSDT", 110);
            return !id.empty() && !bot_of(e, "LIVEUSDT").disaster_stop_id.empty();
        });
        const bool progressed = tick.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
        resume.set_value();
        check(tick.get() && progressed && writer.get(), "常规定时断点写盘慢时行情和已有保护通道仍可推进");
    }
    {
        struct FillRecovery : FakeClient {
            OrderOutcome confirmed_order(const std::string&, const std::string&) override {
                OrderOutcome order;
                order.ok = true; order.order_id = "M1";
                order.executed_qty = 2; order.avg_price = 110;
                return order;
            }
        };
        auto cli = std::make_shared<FillRecovery>();
        TrendEngine restarted(cli, inline_host());
        TrendBot probe;
        probe.cfg = config("KNOWNUSDT");
        probe.ds_probe = probe.open_uncertain = true;
        probe.uncertain_qty = 2; probe.uncertain_stop = 100; probe.uncertain_client_id = "C1";
        probe.reserved_dir = trend::Pos::Long;
        probe.disaster_stop_id = "live-stop";
        probe.disaster_stop_price = 99.8;
        restarted.restore_bot(probe);
        restarted.maintain_paused_hard_stops();
        const auto recovered = bot_of(restarted, "KNOWNUSDT");
        check(recovered.qty == 2 && !recovered.open_uncertain && recovered.ds_probe && cli->stop_log.empty(),
              "未知试探恢复持仓后保留原保护单，等交易所重新确认其存在");
        restarted.reconcile_stop_orders(restarted.capture_stop_orders(), {"live-stop"});
        check(!bot_of(restarted, "KNOWNUSDT").ds_probe && cli->stop_log.empty(),
              "查回试探成交且确认原止损仍活跃后解除熔断，无需重挂");
    }
    {
        auto cli = std::make_shared<FakeClient>();
        TrendEngine e(cli, inline_host());
        trip(e, *cli);
        advance(601);
        signal(e, "UNKNOWNUSDT");
        cli->uncertain_next = true;
        e.tick("UNKNOWNUSDT", 110);
        const auto unknown = bot_of(e, "UNKNOWNUSDT");
        check(unknown.open_uncertain && unknown.ds_probe && unknown.ds_probe_after_ms > 0,
              "订单状态未知时保留试探占用与账户断点");
        TrendEngine restarted(cli, inline_host());
        restarted.restore_bot(unknown);
        signal(restarted, "NEXTUSDT");
        const auto before = openings(*cli);
        restarted.tick("NEXTUSDT", 110);
        check(openings(*cli) == before && bot_of(restarted, "NEXTUSDT").last_decision.find("已有一笔试探") != std::string::npos,
              "重启恢复原试探，不会再发第二笔");
        cli->opening_terminal = true;
        restarted.maintain_paused_hard_stops();
        advance(60);
        restarted.reconcile_positions({}, std::chrono::milliseconds(0));
        restarted.reconcile_positions({}, std::chrono::milliseconds(0));
        check(!bot_of(restarted, "UNKNOWNUSDT").ds_probe, "未成交终态和两次空仓对账后清除探测者");
        restarted.tick("NEXTUSDT", 110);
        check(openings(*cli) == before, "没有确认保护成功的试探重新进入冷却");
        advance(601);
        feed(restarted, bot_of(restarted, "NEXTUSDT").bot_id, 2, 110, 90);
        restarted.tick("NEXTUSDT", 110);
        check(openings(*cli) == before + 1, "新一轮冷却到期允许下一笔试探");
    }
}
