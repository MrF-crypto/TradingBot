#include <barrier>
#include <filesystem>
#include <thread>

static void test_audit_state_scenarios() {
    const std::string path = "audit_state_test.json";
    auto initial = mk_bot("BTCUSDT", trend::Pos::Long,100,111,95,1);
    initial.ds_vanished_id = "77";
    check(save_trend_state(path,{initial}).ok,"atomic checkpoint write succeeds");
    const auto failure = save_trend_state(path+"/cannot-create.json",{initial});
    check(!failure.ok && !failure.error.empty(),"filesystem failure is returned to caller");
    auto got = load_trend_state(path,{mk_cfg("BTCUSDT")});
    check(got.size()==1 && got[0].st.stop==95 && got[0].ds_vanished_id=="77",
          "failed save preserves prior checkpoint and protective order evidence survives restart");
    {
        auto client = std::make_shared<NullClient>(); TrendEngine engine(client,inline_host());
        std::vector<std::string> messages;
        engine.set_log_cb([&](const std::string& message) { messages.push_back(message); });
        const auto id = engine.add_bot(initial.cfg);
        const auto bad = save_trend_state(path+"/cannot-create.json",engine);
        check(!bad.ok && !messages.empty() && messages.back().find("保存失败") != std::string::npos,
              "production writer reports real filesystem errors to the engine");
        engine.stop_bot(id);
        check(save_trend_state(path,engine).ok,"production writer recovers after filesystem becomes writable");
        const auto saved = load_trend_state(path,{initial.cfg});
        check(saved.size()==1 && saved[0].state==TrendBot::State::Stopped,
              "production writer captures the current engine state, including a later manual pause");
    }

    std::vector<TrendBot> a(100,initial), b(100,initial);
    std::vector<TrendConfig> configs;
    for(size_t i=0;i<a.size();++i) {
        a[i].cfg.symbol="SHORT"+std::to_string(i)+"USDT";
        b[i].cfg.symbol="MUCH_LONGER_NAME"+std::to_string(i)+"USDT";
        configs.push_back(a[i].cfg);configs.push_back(b[i].cfg);
    }
    std::barrier barrier(3);std::atomic<int> failures{0};
    const auto writer=[&](const std::vector<TrendBot>& bots){
        for(int i=0;i<40;++i){
            barrier.arrive_and_wait();
            if(!save_trend_state(path,bots).ok) ++failures;
            barrier.arrive_and_wait();
        }
    };
    std::thread first(writer,std::cref(a)),second(writer,std::cref(b));
    for(int i=0;i<40;++i) {
        barrier.arrive_and_wait();barrier.arrive_and_wait();
        try {
            got=load_trend_state(path,configs);
            if(got.size()!=100) ++failures;
            else {
                const bool short_name=got.front().cfg.symbol.starts_with("SHORT");
                for(const auto& bot:got)
                    if(bot.cfg.symbol.starts_with("SHORT")!=short_name) ++failures;
            }
        } catch (...) {++failures;}
    }
    first.join();second.join();
    check(failures.load()==0,"concurrent checkpoint writers always publish one complete snapshot");

    const std::string unicode_path="audit_断点_状态.json";
    check(save_trend_state(unicode_path,{initial}).ok,"checkpoint path supports UTF-8 on all platforms");
    const std::filesystem::path unicode_file(std::u8string(unicode_path.begin(),unicode_path.end()));
    check(std::filesystem::exists(unicode_file),"UTF-8 checkpoint exists under its intended filename");
    check(load_trend_state(unicode_path,{initial.cfg}).size()==1,"UTF-8 checkpoint can be restored");
    std::filesystem::remove(unicode_file);
    std::filesystem::remove(path);
}
