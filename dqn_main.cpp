#include <cmath>
#include <iostream>
#include <ale_interface.hpp>
#include <glog/logging.h>
#include <gflags/gflags.h>
#include "prettyprint.hpp"
#include "dqn.hpp"
#include <boost/filesystem.hpp>
#include <thread>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <limits>

using namespace boost::filesystem;

// DQN Parameters
DEFINE_bool(gpu, true, "Use GPU to brew Caffe");
DEFINE_int32(device, -1, "Which GPU to use");
DEFINE_bool(gui, false, "Open a GUI window");
DEFINE_string(save, "", "Prefix for saving snapshots");
DEFINE_string(rom, "", "Atari 2600 ROM file to play");
DEFINE_int32(memory, 400000, "Capacity of replay memory");
DEFINE_int32(explore, 1000000, "Iterations for epsilon to reach given value.");
DEFINE_double(epsilon, .1, "Value of epsilon after explore iterations.");
DEFINE_double(gamma, .99, "Discount factor of future rewards (0,1]");
DEFINE_int32(clone_freq, 10000, "Frequency (steps) of cloning the target network");
DEFINE_int32(memory_threshold, 50000, "Number of transitions to start learning");
DEFINE_int32(skip_frame, 4, "Number of frames skipped");
DEFINE_int32(update_frequency, 1, "Number of actions between SGD updates");
DEFINE_int32(unroll, 10, "RNN iterations to unroll");
DEFINE_int32(minibatch, 32, "Minibatch size");
DEFINE_int32(frames_per_timestep, 4, "Frames given to agent at each timestep");
DEFINE_string(save_screen, "", "File prefix in to save frames");
DEFINE_string(save_binary_screen, "", "File prefix in to save binary frames");
DEFINE_string(weights, "", "The pretrained weights load (*.caffemodel).");
DEFINE_string(snapshot, "", "The solver state to load (*.solverstate).");
DEFINE_bool(resume, true, "Automatically resume training from latest snapshot.");
DEFINE_bool(evaluate, false, "Evaluation mode: only playing a game, no updates");
DEFINE_double(evaluate_with_epsilon, .05, "Epsilon value to be used in evaluation mode");
DEFINE_int32(evaluate_freq, 50000, "Frequency (steps) between evaluations");
DEFINE_int32(repeat_games, 10, "Number of games played in evaluation mode");
DEFINE_string(solver, "recurrent_solver.prototxt", "Solver parameter file (*.prototxt)");
DEFINE_bool(time, false, "Time the network and exit");
DEFINE_bool(unroll1_is_lstm, false, "Use LSTM layer instead of IP when unroll=1");
DEFINE_int32(obscure_size, 0, "Size of obscured game screen.");

double CalculateEpsilon(const int iter) {
  if (iter < FLAGS_explore) {
    return 1.0 - (1.0 - FLAGS_epsilon) * (static_cast<double>(iter) / FLAGS_explore);
  } else {
    return FLAGS_epsilon;
  }
}

void SaveInputFrame(const dqn::FrameData& frame, const string filename) {
  std::ofstream ofs;
  ofs.open(filename, ios::out | ios::binary);
  for (int i = 0; i < dqn::kCroppedFrameDataSize; ++i) {
    ofs.write((char*) &frame[i], sizeof(uint8_t));
  }
  ofs.close();
}

void InitializeALE(ALEInterface& ale, bool display_screen, std::string& rom) {
  ale.setBool("display_screen", display_screen);
  ale.setBool("sound", display_screen);
  ale.loadROM(rom);
}

/**
 * Play one episode and return the total score
 */
double PlayOneEpisode(ALEInterface& ale, dqn::DQN& dqn, const double epsilon,
                      const bool update) {
  CHECK(!ale.game_over());
  int remaining_lives = ale.lives();
  std::deque<dqn::FrameDataSp> past_frames;
  dqn::Episode episode;
  const ALEScreen* screen = &ale.getScreen();
  dqn::FrameDataSp current_frame = dqn::PreprocessScreen(*screen);
  if (FLAGS_obscure_size > 0) {
    dqn.ObscureScreen(current_frame, FLAGS_obscure_size);
  }
  auto total_score = 0.0;
  bool first_action = true;
  for (auto frame = 0; !ale.game_over(); ++frame) {
    if (!update) { // The next screen will already be populated if doing updates
      screen = &ale.getScreen();
      current_frame = dqn::PreprocessScreen(*screen);
      if (FLAGS_obscure_size > 0) {
        dqn.ObscureScreen(current_frame, FLAGS_obscure_size);
      }
    }
    past_frames.push_back(current_frame);
    if (!FLAGS_save_screen.empty()) {
      std::stringstream ss;
      ss << FLAGS_save_screen << setfill('0') << setw(5) <<
          std::to_string(frame) << ".png";
      ale.saveScreenPNG(ss.str());
    }
    if (!FLAGS_save_binary_screen.empty()) {
      static int binary_save_num = 0;
      string fname = FLAGS_save_binary_screen +
          std::to_string(binary_save_num++) + ".bin";
      SaveInputFrame(*current_frame, fname);
    }
    while (past_frames.size() > FLAGS_frames_per_timestep) {
      past_frames.pop_front();
    }
    CHECK_LE(past_frames.size(), FLAGS_frames_per_timestep);
    Action action = PLAYER_A_NOOP;
    if (past_frames.size() == FLAGS_frames_per_timestep) {
      dqn::InputFrames input_frames(FLAGS_frames_per_timestep);
      std::copy(past_frames.begin(), past_frames.end(), input_frames.begin());
      action = dqn.SelectAction(input_frames, epsilon, !first_action);
      first_action = false;
    }
    auto immediate_score = 0.0;
    for (auto i = 0; i < FLAGS_skip_frame + 1 && !ale.game_over(); ++i) {
      immediate_score += ale.act(action);
    }
    total_score += immediate_score;
    // Rewards for DQN are normalized as follows:
    // 1 for any positive score, -1 for any negative score, otherwise 0
    float reward = immediate_score == 0 ? 0 :
        immediate_score / std::abs(immediate_score);
    if (ale.lives() < remaining_lives) {
      remaining_lives = ale.lives();
      reward = -1;
    }
    assert(reward <= 1 && reward >= -1);
    if (update) {
      // Get the next screen
      screen = &ale.getScreen();
      dqn::FrameDataSp next_frame = dqn::PreprocessScreen(*screen);
      if (FLAGS_obscure_size > 0) {
        dqn.ObscureScreen(current_frame, FLAGS_obscure_size);
      }
      // Add the current transition to replay memory
      const auto transition = ale.game_over() ?
          dqn::Transition(current_frame, action, reward, boost::none) :
          dqn::Transition(current_frame, action, reward, next_frame);
      episode.push_back(transition);
      if (dqn.memory_size() > FLAGS_memory_threshold
          && frame % FLAGS_update_frequency == 0) {
        dqn.UpdateRandom();
      }
      current_frame = next_frame;
    }
  }
  if (update) {
    dqn.RememberEpisode(episode);
  }
  ale.reset_game();
  return total_score;
}

/**
 * Evaluate the current player
 */
double Evaluate(ALEInterface& ale, dqn::DQN& dqn) {
  // std::vector<double> scores = PlayParallelEpisodes(
  //     dqn, FLAGS_evaluate_with_epsilon, false);
  std::vector<double> scores;
  scores.push_back(PlayOneEpisode(ale, dqn, FLAGS_evaluate_with_epsilon, false));
  double total_score = 0.0;
  for (auto score : scores) {
    total_score += score;
  }
  const auto avg_score = total_score / static_cast<double>(scores.size());
  double stddev = 0.0; // Compute the sample standard deviation
  for (auto i=0; i<scores.size(); ++i) {
    stddev += (scores[i] - avg_score) * (scores[i] - avg_score);
  }
  stddev = sqrt(stddev / static_cast<double>(FLAGS_repeat_games - 1));
  LOG(INFO) << "Evaluation avg_score = " << avg_score << " std = " << stddev;
  return avg_score;
}

int main(int argc, char** argv) {
 std::string usage(argv[0]);
  usage.append(" -rom rom -[evaluate|save path]");
  gflags::SetUsageMessage(usage);
  gflags::SetVersionString("0.1");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();
  if (FLAGS_evaluate) {
    google::LogToStderr();
  }
  if (FLAGS_rom.empty()) {
    LOG(ERROR) << "Rom file required but not set.";
    LOG(ERROR) << "Usage: " << gflags::ProgramUsage();
    exit(1);
  }
  path rom_file(FLAGS_rom);
  if (!is_regular_file(rom_file)) {
    LOG(ERROR) << "Invalid ROM file: " << FLAGS_rom;
    exit(1);
  }
  if (!is_regular_file(FLAGS_solver)) {
    LOG(ERROR) << "Invalid solver: " << FLAGS_solver;
    exit(1);
  }
  if (FLAGS_save.empty() && !FLAGS_evaluate) {
    LOG(ERROR) << "Save path (or evaluate) required but not set.";
    LOG(ERROR) << "Usage: " << gflags::ProgramUsage();
    exit(1);
  }
  path save_path(FLAGS_save);
  if (!FLAGS_evaluate) {
    path snapshot_dir(current_path());
    if (is_directory(save_path)) {
      snapshot_dir = save_path;
      save_path /= rom_file.stem();
    } else {
      if (save_path.has_parent_path()) {
        snapshot_dir = save_path.parent_path();
      }
      save_path += "_";
      save_path += rom_file.stem();
    }
    // Set the logging destinations
    google::SetLogDestination(google::GLOG_INFO,
                              (save_path.native() + "_INFO_").c_str());
    google::SetLogDestination(google::GLOG_WARNING,
                              (save_path.native() + "_WARNING_").c_str());
    google::SetLogDestination(google::GLOG_ERROR,
                              (save_path.native() + "_ERROR_").c_str());
    google::SetLogDestination(google::GLOG_FATAL,
                              (save_path.native() + "_FATAL_").c_str());
  }

  if (FLAGS_gpu) {
    caffe::Caffe::set_mode(caffe::Caffe::GPU);
    if (FLAGS_device >= 0) {
      caffe::Caffe::SetDevice(FLAGS_device);
    }
  } else {
    caffe::Caffe::set_mode(caffe::Caffe::CPU);
  }

  // Look for a recent snapshot to resume
  if (FLAGS_resume && FLAGS_snapshot.empty()) {
    FLAGS_snapshot = dqn::FindLatestSnapshot(save_path.native());
  }

  ALEInterface ale;
  InitializeALE(ale, FLAGS_gui, FLAGS_rom);

  // Get the vector of legal actions
  const auto legal_actions = ale.getMinimalActionSet();

  CHECK(FLAGS_snapshot.empty() || FLAGS_weights.empty())
      << "Give a snapshot to resume training or weights to finetune "
      "but not both.";

  dqn::DQN dqn(legal_actions, FLAGS_memory, FLAGS_gamma,
               FLAGS_clone_freq, FLAGS_unroll, FLAGS_minibatch,
               FLAGS_frames_per_timestep);

  // Construct the solver
  caffe::SolverParameter solver_param;
  caffe::ReadProtoFromTextFileOrDie(FLAGS_solver, &solver_param);
  caffe::NetParameter* net_param = solver_param.mutable_net_param();
  net_param->CopyFrom(dqn.CreateNet(FLAGS_unroll1_is_lstm));
  std::string net_filename = save_path.native() + "_net.prototxt";
  WriteProtoToTextFile(*net_param, net_filename.c_str());
  solver_param.set_snapshot_prefix(save_path.c_str());

  dqn.Initialize(solver_param);

  if (!FLAGS_save_screen.empty()) {
    LOG(INFO) << "Saving screens to: " << FLAGS_save_screen;
  }

  if (!FLAGS_snapshot.empty()) {
    path p(FLAGS_snapshot);
    p = p.parent_path() / p.stem();
    std::string mem_fname = p.native() + ".replaymemory";
    CHECK(is_regular_file(mem_fname))
        << "Unable to find .replaymemory for snapshot: " << FLAGS_snapshot;
    LOG(INFO) << "Resuming from " << FLAGS_snapshot;
    dqn.RestoreSolver(FLAGS_snapshot);
    dqn.LoadReplayMemory(mem_fname);
  } else if (!FLAGS_weights.empty()) {
    LOG(INFO) << "Finetuning from " << FLAGS_weights;
    dqn.LoadTrainedModel(FLAGS_weights);
  }

  if (FLAGS_evaluate) {
    if (FLAGS_gui) {
      auto score = PlayOneEpisode(ale, dqn, FLAGS_evaluate_with_epsilon, false);
      LOG(INFO) << "Score " << score;
    } else {
      Evaluate(ale, dqn);
    }
    return 0;
  }

  if (FLAGS_time) {
    auto score = PlayOneEpisode(ale, dqn, FLAGS_evaluate_with_epsilon, true);
    dqn.Benchmark();
    return 0;
  }

  int last_eval_iter = 0;
  int episode = 0;
  double best_score = std::numeric_limits<double>::lowest();
  if (FLAGS_resume) {
    best_score = dqn::FindHiScore(save_path.native());
    LOG(INFO) << "Resuming from HiScore " << best_score;
  }
  while (dqn.current_iteration() < solver_param.max_iter()) {
    double epsilon = CalculateEpsilon(dqn.current_iteration());
    double score = PlayOneEpisode(ale, dqn, epsilon, true);
    int iter = dqn.current_iteration();
    LOG(INFO) << "Episode " << episode << " score = " << score
              << ", epsilon = " << epsilon
              << ", iter = " << iter
              << ", replay_mem_size = " << dqn.memory_size();
    episode++;
    if ((score > best_score && iter >= FLAGS_explore) ||
        dqn.current_iteration() >= last_eval_iter + FLAGS_evaluate_freq) {
      double avg_score = Evaluate(ale, dqn);
      if (avg_score > best_score) {
        LOG(INFO) << "iter " << dqn.current_iteration()
                  << " New High Score: " << avg_score;
        best_score = avg_score;
        std::string fname = save_path.native() + "_HiScore" +
            std::to_string(int(avg_score));
        dqn.Snapshot(fname, false, false);
      }
      dqn.Snapshot(save_path.native(), true, true);
      last_eval_iter = dqn.current_iteration();
    }
  }
  if (dqn.current_iteration() >= last_eval_iter) {
    Evaluate(ale, dqn);
    dqn.Snapshot(save_path.native(), true, true);
  }
}
