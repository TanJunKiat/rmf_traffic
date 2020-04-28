/*
 * Copyright (C) 2020 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#ifndef RMF_TRAFFIC__TEST__UNIT__AGV__UTILS_NEGOTIATIONROOM_HPP
#define RMF_TRAFFIC__TEST__UNIT__AGV__UTILS_NEGOTIATIONROOM_HPP

#include <rmf_traffic/agv/Negotiator.hpp>
#include <rmf_traffic/schedule/Negotiation.hpp>
#include <rmf_traffic/schedule/Participant.hpp>

#include <rmf_utils/catch.hpp>

#include <unordered_map>
#include <future>
#include <iostream>
#include <deque>

//==============================================================================
class NegotiationRoom
{
public:

  using ParticipantId = rmf_traffic::schedule::ParticipantId;
  using Negotiator = rmf_traffic::agv::SimpleNegotiator;
  using Negotiation = rmf_traffic::schedule::Negotiation;
  using Responder = rmf_traffic::schedule::SimpleResponder;

  struct Intention
  {
    std::vector<rmf_traffic::agv::Planner::Start> start;
    rmf_traffic::agv::Planner::Goal goal;
    rmf_traffic::agv::Planner::Configuration configuration;

    Intention(
      std::vector<rmf_traffic::agv::Planner::Start> starts_,
      rmf_traffic::agv::Planner::Goal goal_,
      rmf_traffic::agv::Planner::Configuration config_)
    : start(std::move(starts_)),
      goal(std::move(goal_)),
      configuration(std::move(config_))
    {
      // Do nothing
    }

    Intention(
      rmf_traffic::agv::Planner::Start start_,
      rmf_traffic::agv::Planner::Goal goal_,
      rmf_traffic::agv::Planner::Configuration config_)
    : start({std::move(start_)}),
      goal(std::move(goal_)),
      configuration(std::move(config_))
    {
      // Do nothing
    }
  };

  using Intentions = std::unordered_map<ParticipantId, Intention>;

  NegotiationRoom(
    const rmf_traffic::schedule::Viewer& viewer,
    Intentions intentions,
    const bool print_failures_ = false)
  : negotiators(make_negotiations(intentions)),
    negotiation(std::make_shared<Negotiation>(
        viewer, get_participants(intentions))),
    _print(print_failures_)
  {
    // Do nothing
  }

  static bool skip(const Negotiation::TablePtr& table)
  {
    if (table->submission() && !table->rejected())
      return true;

    auto ancestor = table->parent();
    while (ancestor)
    {
      if (ancestor->rejected() || ancestor->forfeited())
        return true;

      ancestor = ancestor->parent();
    }

    return false;
  }

  rmf_utils::optional<Negotiation::Proposal> solve()
  {
    std::deque<Negotiation::TablePtr> queue;

    for (const auto& p : negotiation->participants())
    {
      const auto table = negotiation->table(p, {});
      queue.push_back(table);
    }

    while (!queue.empty() && !negotiation->ready())
    {
      std::cout << "Queue size: " << queue.size() << std::endl;
      const auto top = queue.back();
      queue.pop_back();

      if (skip(top))
      {
        // An ancestor has been rejected since this table was originally made,
        // so we should pass to let the parent be fixed.
        if (_print)
        {
          std::cout << "Skipping [";
          for (const auto p : top->sequence())
            std::cout << " " << p;
          std::cout << " ]" << std::endl;
        }
        continue;
      }

      if (_print)
      {
        std::cout << "Responding to [";
        for (const auto p : top->sequence())
          std::cout << " " << p;
        std::cout << " ]" << std::endl;
      }

      auto& negotiator = negotiators.at(top->participant());

      bool interrupt = false;
      auto result = std::async(
            std::launch::async,
            [&]()
      {
        negotiator.respond(
              top, Responder(negotiation, top->sequence()), &interrupt);
      });

      using namespace std::chrono_literals;
      // Give up if the solution is not found within a time limit
      if (result.wait_for(10s) != std::future_status::ready)
        interrupt = true;

      result.wait();

      if (top->submission())
      {
        if (_print)
        {
          std::cout << "Submission given for [";
          for (const auto p : top->sequence())
            std::cout << " " << p;
          std::cout << " ]" << std::endl;
        }

        for (const auto& n : negotiators)
        {
          const auto participant = n.first;
          const auto respond_to = top->respond(participant);
          if (respond_to)
            queue.push_back(respond_to);
        }

        continue;
      }

      const auto parent = top->parent();
      if (parent && parent->rejected())
      {
        if (_print)
        {
          std::cout << "[" << std::to_string(top->participant())
                    << "] rejected [";
          for (const auto p : parent->sequence())
            std::cout << " " << p;
          std::cout << " ]" << std::endl;
        }
        queue.push_back(parent);
      }

      if (top->forfeited())
      {
        if (_print)
        {
          std::cout << "Forfeit given for [";
          for (const auto p : top->sequence())
            std::cout << " " << p;
          std::cout << " ]" << std::endl;
        }
      }
    }

//    CHECK(negotiation->complete());
    if (!negotiation->ready())
      return rmf_utils::nullopt;

    return negotiation->evaluate(
      rmf_traffic::schedule::QuickestFinishEvaluator())->proposal();
  }

  static std::unordered_map<ParticipantId, Negotiator> make_negotiations(
    const std::unordered_map<ParticipantId, Intention>& intentions)
  {
    std::unordered_map<ParticipantId, Negotiator> negotiators;
    for (const auto& entry : intentions)
    {
      const auto participant = entry.first;
      const auto& intention = entry.second;
      negotiators.insert(
        std::make_pair(
          participant,
          rmf_traffic::agv::SimpleNegotiator(
            intention.start, intention.goal, intention.configuration)));
    }

    return negotiators;
  }

  static std::vector<ParticipantId> get_participants(
    const std::unordered_map<ParticipantId, Intention>& intentions)
  {
    std::vector<ParticipantId> participants;
    participants.reserve(intentions.size());
    for (const auto& entry : intentions)
      participants.push_back(entry.first);

    return participants;
  }

  std::unordered_map<ParticipantId, Negotiator> negotiators;
  std::shared_ptr<rmf_traffic::schedule::Negotiation> negotiation;

  NegotiationRoom& print()
  {
    _print = true;
    return *this;
  }

  bool _print = false;
};

#endif // UTILS_NEGOTIATIONROOM_HPP
