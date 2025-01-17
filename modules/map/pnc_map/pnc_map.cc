/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/map/pnc_map/pnc_map.h"

#include <algorithm>
#include <limits>

#include "absl/strings/str_cat.h"
#include "google/protobuf/text_format.h"

#include "modules/map/proto/map_id.pb.h"

#include "cyber/common/log.h"
#include "modules/common/util/point_factory.h"
#include "modules/common/util/string_util.h"
#include "modules/common/util/util.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/map/pnc_map/path.h"
#include "modules/planning/common/planning_gflags.h"
#include "modules/routing/common/routing_gflags.h"

DEFINE_double(
    look_backward_distance, 50,
    "look backward this distance when creating reference line from routing");

DEFINE_double(look_forward_short_distance, 180,
              "short look forward this distance when creating reference line "
              "from routing when ADC is slow");
DEFINE_double(
    look_forward_long_distance, 250,
    "look forward this distance when creating reference line from routing");

namespace apollo {
namespace hdmap {

using apollo::common::PointENU;
using apollo::common::VehicleState;
using apollo::common::util::PointFactory;
using apollo::routing::RoutingResponse;

namespace {

// Maximum lateral error used in trajectory approximation.
const double kTrajectoryApproximationMaxError = 2.0;

}  // namespace

PncMap::PncMap(const HDMap *hdmap) : hdmap_(hdmap) {}

const hdmap::HDMap *PncMap::hdmap() const { return hdmap_; }


//waypint转换为lanewaypoint
LaneWaypoint PncMap::ToLaneWaypoint(
    const routing::LaneWaypoint &waypoint) const {
  auto lane = hdmap_->GetLaneById(hdmap::MakeMapId(waypoint.id()));
  ACHECK(lane) << "Invalid lane id: " << waypoint.id();
  return LaneWaypoint(lane, waypoint.s());
}

//求取前向距离
double PncMap::LookForwardDistance(double velocity) {
  //时间（8秒）*车速
  auto forward_distance = velocity * FLAGS_look_forward_time_sec;
  //最大不超过180m
  return forward_distance > FLAGS_look_forward_short_distance
             ? FLAGS_look_forward_long_distance
             : FLAGS_look_forward_short_distance;
}


LaneSegment PncMap::ToLaneSegment(const routing::LaneSegment &segment) const {
  auto lane = hdmap_->GetLaneById(hdmap::MakeMapId(segment.id()));
  ACHECK(lane) << "Invalid lane id: " << segment.id();
  return LaneSegment(lane, segment.start_s(), segment.end_s());
}

//更新下个RoutingWaypoint序号
void PncMap::UpdateNextRoutingWaypointIndex(int cur_index) {
  //如果是起始点-1，则下个点next_routing_waypoint_index_=0
  if (cur_index < 0) {
    next_routing_waypoint_index_ = 0;
    return;
  }
  //如果大于route_indices_的数量，则返回最大点
  if (cur_index >= static_cast<int>(route_indices_.size())) {
    next_routing_waypoint_index_ = routing_waypoint_index_.size() - 1;
    return;
  }
  //如果是倒车，则往后寻找
  // Search backwards when the car is driven backward on the route.
  while (next_routing_waypoint_index_ != 0 &&
         next_routing_waypoint_index_ < routing_waypoint_index_.size() &&
         routing_waypoint_index_[next_routing_waypoint_index_].index >
             cur_index) {
    --next_routing_waypoint_index_;
  }
  while (next_routing_waypoint_index_ != 0 &&
         next_routing_waypoint_index_ < routing_waypoint_index_.size() &&
         routing_waypoint_index_[next_routing_waypoint_index_].index ==
             cur_index &&
         adc_waypoint_.s <
             routing_waypoint_index_[next_routing_waypoint_index_].waypoint.s) {
    --next_routing_waypoint_index_;
  }
  //如果是前进，则向前搜索
  // Search forwards
  while (next_routing_waypoint_index_ < routing_waypoint_index_.size() &&
         routing_waypoint_index_[next_routing_waypoint_index_].index <
             cur_index) {
    ++next_routing_waypoint_index_;
  }
  while (next_routing_waypoint_index_ < routing_waypoint_index_.size() &&
         cur_index ==
             routing_waypoint_index_[next_routing_waypoint_index_].index &&
         adc_waypoint_.s >=
             routing_waypoint_index_[next_routing_waypoint_index_].waypoint.s) {
    ++next_routing_waypoint_index_;
  }
  //如果超过范围，则返回最大值
  if (next_routing_waypoint_index_ >= routing_waypoint_index_.size()) {
    next_routing_waypoint_index_ = routing_waypoint_index_.size() - 1;
  }
}

std::vector<routing::LaneWaypoint> PncMap::FutureRouteWaypoints() const {
  const auto &waypoints = routing_.routing_request().waypoint();
  return std::vector<routing::LaneWaypoint>(
      waypoints.begin() + next_routing_waypoint_index_, waypoints.end());
}

//完成对rang_lane_ids_de 更新，以及起始range_start_，结束点range_end_进行更新
void PncMap::UpdateRoutingRange(int adc_index) {
  // Track routing range.
  range_lane_ids_.clear();
  range_start_ = std::max(0, adc_index - 1);
  range_end_ = range_start_;
  while (range_end_ < static_cast<int>(route_indices_.size())) {
    const auto &lane_id = route_indices_[range_end_].segment.lane->id().id();
    if (range_lane_ids_.count(lane_id) != 0) {
      break;
    }
    range_lane_ids_.insert(lane_id);
    ++range_end_;
  }
}

bool PncMap::UpdateVehicleState(const VehicleState &vehicle_state) {
  if (!ValidateRouting(routing_)) {
    AERROR << "The routing is invalid when updating vehicle state.";
    return false;
  }
  //车辆位已经变化，但是没有规划没有重新更新，
  if (!adc_state_.has_x() ||
      (common::util::DistanceXY(adc_state_, vehicle_state) >
       FLAGS_replan_lateral_distance_threshold +
           FLAGS_replan_longitudinal_distance_threshold)) {
    // Position is reset, but not replan.
    next_routing_waypoint_index_ = 0;
    adc_route_index_ = -1;
    stop_for_destination_ = false;
  }
  //更新车辆位置到成员变量adc_state_
  adc_state_ = vehicle_state;
  //根据vehicle_state获取与车辆最近的waypoint点
  if (!GetNearestPointFromRouting(vehicle_state, &adc_waypoint_)) {
    AERROR << "Failed to get waypoint from routing with point: "
           << "(" << vehicle_state.x() << ", " << vehicle_state.y() << ", "
           << vehicle_state.z() << ").";
    return false;
  }
  //根据adc_waypoint_确认已经超过的route点
  int route_index = GetWaypointIndex(adc_waypoint_);
  //如果序号<0或者大于道路前向道路size，返回错误信息
  if (route_index < 0 ||
      route_index >= static_cast<int>(route_indices_.size())) {
    AERROR << "Cannot find waypoint: " << adc_waypoint_.DebugString();
    return false;
  }
  //更新下一个rout点
  // Track how many routing request waypoints the adc have passed.
  UpdateNextRoutingWaypointIndex(route_index);
  adc_route_index_ = route_index;
  //获取剩余的routing路线
  UpdateRoutingRange(adc_route_index_);
  //没有routing信息
  if (routing_waypoint_index_.empty()) {
    AERROR << "No routing waypoint index.";
    return false;
  }
  //如果到了最后一个点，则停车标志位置true
  if (next_routing_waypoint_index_ == routing_waypoint_index_.size() - 1) {
    stop_for_destination_ = true;
  }
  return true;
}

//是否是新的routing，判断新的routing和之前存储的routing进行对比
bool PncMap::IsNewRouting(const routing::RoutingResponse &routing) const {
  return IsNewRouting(routing_, routing);
}

bool PncMap::IsNewRouting(const routing::RoutingResponse &prev,
                          const routing::RoutingResponse &routing) {
  if (!ValidateRouting(routing)) {
    ADEBUG << "The provided routing is invalid.";
    return false;
  }
  //用proto是否相同来判断
  return !common::util::IsProtoEqual(prev, routing);
}


//对routing信息进行更新
bool PncMap::UpdateRoutingResponse(const routing::RoutingResponse &routing) {
  //清除range_lane_ids_，route_indices_，all_lane_ids_三个成员变量的内容
  range_lane_ids_.clear();
  route_indices_.clear();
  all_lane_ids_.clear();
  //遍历所有的road和segement，和passage
  for (int road_index = 0; road_index < routing.road_size(); ++road_index) {
    const auto &road_segment = routing.road(road_index);
    for (int passage_index = 0; passage_index < road_segment.passage_size();
         ++passage_index) {
      const auto &passage = road_segment.passage(passage_index);
      for (int lane_index = 0; lane_index < passage.segment_size();
           ++lane_index) {
        all_lane_ids_.insert(passage.segment(lane_index).id());
        route_indices_.emplace_back();
        route_indices_.back().segment =
            ToLaneSegment(passage.segment(lane_index));
        if (route_indices_.back().segment.lane == nullptr) {
          AERROR << "Failed to get lane segment from passage.";
          return false;
        }
        route_indices_.back().index = {road_index, passage_index, lane_index};
      }
    }
  }

  range_start_ = 0;
  range_end_ = 0;
  adc_route_index_ = -1;
  next_routing_waypoint_index_ = 0;
  UpdateRoutingRange(adc_route_index_);

  routing_waypoint_index_.clear();
  const auto &request_waypoints = routing.routing_request().waypoint();
  if (request_waypoints.empty()) {
    AERROR << "Invalid routing: no request waypoints.";
    return false;
  }
  int i = 0;
  //遍历，寻找在waypoints和route_indices_，更新routing_waypoint_index_
  for (size_t j = 0; j < route_indices_.size(); ++j) {
    while (i < request_waypoints.size() &&
           RouteSegments::WithinLaneSegment(route_indices_[j].segment,
                                            request_waypoints.Get(i))) {
      routing_waypoint_index_.emplace_back(
          LaneWaypoint(route_indices_[j].segment.lane,
                       request_waypoints.Get(i).s()),
          j);
      ++i;
    }
  }
  routing_ = routing;
  adc_waypoint_ = LaneWaypoint();
  stop_for_destination_ = false;
  return true;
}

const routing::RoutingResponse &PncMap::routing_response() const {
  return routing_;
}


//判断routing是否有效
bool PncMap::ValidateRouting(const RoutingResponse &routing) {
  //确认routing中道路的数量，如果等于0，则返回无效
  const int num_road = routing.road_size();
  if (num_road == 0) {
    AERROR << "Route is empty.";
    return false;
  }
  //如果routing点数量小于2，则返回无效
  if (!routing.has_routing_request() ||
      routing.routing_request().waypoint_size() < 2) {
    AERROR << "Routing does not have request.";
    return false;
  }
  //进一步确认waypoint的信息是否有效，wapypint是否包含有效id，是否包含s信息
  for (const auto &waypoint : routing.routing_request().waypoint()) {
    if (!waypoint.has_id() || !waypoint.has_s()) {
      AERROR << "Routing waypoint has no lane_id or s.";
      return false;
    }
  }
  return true;
}

//前向搜索index
int PncMap::SearchForwardWaypointIndex(int start,
                                       const LaneWaypoint &waypoint) const {
  int i = std::max(start, 0);
  while (
      i < static_cast<int>(route_indices_.size()) &&
      !RouteSegments::WithinLaneSegment(route_indices_[i].segment, waypoint)) {
    ++i;
  }
  return i;
}

//后向搜索index
int PncMap::SearchBackwardWaypointIndex(int start,
                                        const LaneWaypoint &waypoint) const {
  int i = std::min(static_cast<int>(route_indices_.size() - 1), start);
  while (i >= 0 && !RouteSegments::WithinLaneSegment(route_indices_[i].segment,
                                                     waypoint)) {
    --i;
  }
  return i;
}

int PncMap::NextWaypointIndex(int index) const {
  if (index >= static_cast<int>(route_indices_.size() - 1)) {
    return static_cast<int>(route_indices_.size()) - 1;
  } else if (index < 0) {
    return 0;
  } else {
    return index + 1;
  }
}


int PncMap::GetWaypointIndex(const LaneWaypoint &waypoint) const {
  //根据车辆所在位置，确认其在rout point的序号
  int forward_index = SearchForwardWaypointIndex(adc_route_index_, waypoint);
  if (forward_index >= static_cast<int>(route_indices_.size())) {
    return SearchBackwardWaypointIndex(adc_route_index_, waypoint);
  }
  if (forward_index == adc_route_index_ ||
      forward_index == adc_route_index_ + 1) {
    return forward_index;
  }
  auto backward_index = SearchBackwardWaypointIndex(adc_route_index_, waypoint);
  if (backward_index < 0) {
    return forward_index;
  }

  return (backward_index + 1 == adc_route_index_) ? backward_index
                                                  : forward_index;
}

bool PncMap::PassageToSegments(routing::Passage passage,
                               RouteSegments *segments) const {
  CHECK_NOTNULL(segments);
  segments->clear();
  //遍历passage中所有的segment
  for (const auto &lane : passage.segment()) {
    //根据id获取对应的指针
    auto lane_ptr = hdmap_->GetLaneById(hdmap::MakeMapId(lane.id()));
    if (!lane_ptr) {
      AERROR << "Failed to find lane: " << lane.id();
      return false;
    }
    //定义起始s和结束s
    segments->emplace_back(lane_ptr, std::max(0.0, lane.start_s()),
                           std::min(lane_ptr->total_length(), lane.end_s()));
  }
  return !segments->empty();
}

std::vector<int> PncMap::GetNeighborPassages(const routing::RoadSegment &road,
                                             int start_passage) const {
  CHECK_GE(start_passage, 0);
  CHECK_LE(start_passage, road.passage_size());
  std::vector<int> result;
  //根据车辆位置序号，获取passage信息
  const auto &source_passage = road.passage(start_passage);
  result.emplace_back(start_passage);
  //如果当前车道变道类型是FORWARD，返回当前车道
  if (source_passage.change_lane_type() == routing::FORWARD) {
    return result;
  }
  //如果当前车道可以退出  疑问点：can_exit代表什么
  if (source_passage.can_exit()) {  // No need to change lane
    return result;
  }
  RouteSegments source_segments;
  //获取当前车道segments
  if (!PassageToSegments(source_passage, &source_segments)) {
    AERROR << "Failed to convert passage to segments";
    return result;
  }
  //如果下一个routing点在当前passage上，则退出
  if (next_routing_waypoint_index_ < routing_waypoint_index_.size() &&
      source_segments.IsWaypointOnSegment(
          routing_waypoint_index_[next_routing_waypoint_index_].waypoint)) {
    ADEBUG << "Need to pass next waypoint[" << next_routing_waypoint_index_
           << "] before change lane";
    return result;
  }
  //遍历所有的segments，获取左侧的左右车道 疑问点：会将左侧或者右侧车道都找出来么？
  std::unordered_set<std::string> neighbor_lanes;
  if (source_passage.change_lane_type() == routing::LEFT) {
    for (const auto &segment : source_segments) {
      for (const auto &left_id :
           segment.lane->lane().left_neighbor_forward_lane_id()) {
        neighbor_lanes.insert(left_id.id());
      }
    }
  } else if (source_passage.change_lane_type() == routing::RIGHT) {
    for (const auto &segment : source_segments) {
      for (const auto &right_id :
           segment.lane->lane().right_neighbor_forward_lane_id()) {
        neighbor_lanes.insert(right_id.id());
      }
    }
  }
  //neighbor_lanes中如果有target_passage，则将序号放进result中
  for (int i = 0; i < road.passage_size(); ++i) {
    if (i == start_passage) {
      continue;
    }
    const auto &target_passage = road.passage(i);
    for (const auto &segment : target_passage.segment()) {
      if (neighbor_lanes.count(segment.id())) {
        result.emplace_back(i);
        break;
      }
    }
  }
  return result;
}

bool PncMap::GetRouteSegments(const VehicleState &vehicle_state,
                              std::list<RouteSegments> *const route_segments) {
  //根据车速求取前向距离                      
  double look_forward_distance =
      LookForwardDistance(vehicle_state.linear_velocity());
  //默认向后距离
  double look_backward_distance = FLAGS_look_backward_distance;
  return GetRouteSegments(vehicle_state, look_backward_distance,
                          look_forward_distance, route_segments);
}

bool PncMap::GetRouteSegments(const VehicleState &vehicle_state,
                              const double backward_length,
                              const double forward_length,
                              std::list<RouteSegments> *const route_segments) {
  //根据车辆状态更新相关成员变量信息
  if (!UpdateVehicleState(vehicle_state)) {
    AERROR << "Failed to update vehicle state in pnc_map.";
    return false;
  }
  // Vehicle has to be this close to lane center before considering change
  // lane
  //如果adc_waypoint没有道路信息，或者_adc_route_index_无效，返回错误信息
  if (!adc_waypoint_.lane || adc_route_index_ < 0 ||
      adc_route_index_ >= static_cast<int>(route_indices_.size())) {
    AERROR << "Invalid vehicle state in pnc_map, update vehicle state first.";
    return false;
  }

  const auto &route_index = route_indices_[adc_route_index_].index;
  const int road_index = route_index[0];//道路序号
  const int passage_index = route_index[1];//passage序号
  const auto &road = routing_.road(road_index);//根据道路序号获取道路信息
  // Raw filter to find all neighboring passages
  //获取相邻车道的index
  auto drive_passages = GetNeighborPassages(road, passage_index);//后面会对所有drive_passages进行帅选，不符合条件的就会剔除
  for (const int index : drive_passages) {
    //根据车道index获取车道信息
    const auto &passage = road.passage(index);
    RouteSegments segments;
    //提取本次循环passage中的segement信息
    if (!PassageToSegments(passage, &segments)) {
      ADEBUG << "Failed to convert passage to lane segments.";
      continue;
    }
    //获取最近的投影点
    const PointENU nearest_point =
        index == passage_index
            ? adc_waypoint_.lane->GetSmoothPoint(adc_waypoint_.s)//直接根据s值获取投影点
            : PointFactory::ToPointENU(adc_state_);//疑问点？需要进一步研究
    common::SLPoint sl;
    LaneWaypoint segment_waypoint;
    if (!segments.GetProjection(nearest_point, &sl, &segment_waypoint)) {
      ADEBUG << "Failed to get projection from point: "
             << nearest_point.ShortDebugString();
      continue;
    }
    //如果当非车辆当前车道，判断车辆是否可以驶入
    if (index != passage_index) {
      if (!segments.CanDriveFrom(adc_waypoint_)) {
        ADEBUG << "You cannot drive from current waypoint to passage: "
               << index;
        continue;//跳入下个循环
      }
    }
    //route_segments中添加新的元素
    route_segments->emplace_back();
    //获取segments中最后一个点的位置
    const auto last_waypoint = segments.LastWaypoint();
    //对segment进行拓展
    if (!ExtendSegments(segments, sl.s() - backward_length,
                        sl.s() + forward_length, &route_segments->back())) {
      AERROR << "Failed to extend segments with s=" << sl.s()
             << ", backward: " << backward_length
             << ", forward: " << forward_length;
      return false;
    }
    if (route_segments->back().IsWaypointOnSegment(last_waypoint)) {
      route_segments->back().SetRouteEndWaypoint(last_waypoint);
    }
    //对route_segments最后添加进来的元素进行属性更新
    route_segments->back().SetCanExit(passage.can_exit());//可以驶出
    route_segments->back().SetNextAction(passage.change_lane_type());//换道方式（左换道，右换道）
    const std::string route_segment_id = absl::StrCat(road_index, "_", index);
    route_segments->back().SetId(route_segment_id);
    route_segments->back().SetStopForDestination(stop_for_destination_);//设置是否停车的标志位
    if (index == passage_index) {
      route_segments->back().SetIsOnSegment(true);
      route_segments->back().SetPreviousAction(routing::FORWARD);//设置前置动作为forward，直行
    } else if (sl.l() > 0) {
      route_segments->back().SetPreviousAction(routing::RIGHT);//设置前置动作为right，右转
    } else {
      route_segments->back().SetPreviousAction(routing::LEFT);//设置前置动作为left，左转
    }
  }
  return !route_segments->empty();
}

bool PncMap::GetNearestPointFromRouting(const VehicleState &state,
                                        LaneWaypoint *waypoint) const {
  const double kMaxDistance = 10.0;  // meters.//距离偏差要求
  const double kHeadingBuffer = M_PI / 10.0;   //朝向角偏差要求
  waypoint->lane = nullptr;
  std::vector<LaneInfoConstPtr> lanes;
  const auto point = PointFactory::ToPointENU(state);//获取车辆位置x,y,z位置
  const int status =
      hdmap_->GetLanesWithHeading(point, kMaxDistance, state.heading(),
                                  M_PI / 2.0 + kHeadingBuffer, &lanes);//获取距离和朝向角在要求范围内的lanes
  ADEBUG << "lanes:" << lanes.size();
  if (status < 0) {
    AERROR << "Failed to get lane from point: " << point.ShortDebugString();
    return false;
  }
  if (lanes.empty()) {
    AERROR << "No valid lane found within " << kMaxDistance
           << " meters with heading " << state.heading();
    return false;
  }
  //
  std::vector<LaneInfoConstPtr> valid_lanes;
  //lambda，将在range_lane_ids_的id赋值到valid_lanes里
  std::copy_if(lanes.begin(), lanes.end(), std::back_inserter(valid_lanes),
               [&](LaneInfoConstPtr ptr) {
                 return range_lane_ids_.count(ptr->lane().id().id()) > 0;
               });
  //如果valid_lanes为空，将在all_lane_ids_的id赋值到valid_lanes里
  if (valid_lanes.empty()) {
    std::copy_if(lanes.begin(), lanes.end(), std::back_inserter(valid_lanes),
                 [&](LaneInfoConstPtr ptr) {
                   return all_lane_ids_.count(ptr->lane().id().id()) > 0;
                 });
  }

  // Get nearest_waypoints for current position
  double min_distance = std::numeric_limits<double>::infinity();
  //在有效lanes中查找与车辆位置最近的点，并将距离最小的lane作为车辆所在的lane
  for (const auto &lane : valid_lanes) {
    if (range_lane_ids_.count(lane->id().id()) == 0) {
      continue;
    }
    {
      double s = 0.0;
      double l = 0.0;
      //查找投影点
      if (!lane->GetProjection({point.x(), point.y()}, &s, &l)) {
        AERROR << "fail to get projection";
        return false;
      }
      // Use large epsilon to allow projection diff
      static constexpr double kEpsilon = 0.5;
      if (s > (lane->total_length() + kEpsilon) || (s + kEpsilon) < 0.0) {
        continue;
      }
    }
    double distance = 0.0;
    //查找距离最小的点
    common::PointENU map_point =
        lane->GetNearestPoint({point.x(), point.y()}, &distance);
    if (distance < min_distance) {
      min_distance = distance;
      double s = 0.0;
      double l = 0.0;
      //查找最近点的投影点
      if (!lane->GetProjection({map_point.x(), map_point.y()}, &s, &l)) {
        AERROR << "Failed to get projection for map_point: "
               << map_point.DebugString();
        return false;
      }
      waypoint->lane = lane;
      waypoint->s = s;
    }
    ADEBUG << "distance" << distance;
  }
  if (waypoint->lane == nullptr) {
    AERROR << "Failed to find nearest point: " << point.ShortDebugString();
  }
  return waypoint->lane != nullptr;
}

//求后一个连接lane id
LaneInfoConstPtr PncMap::GetRouteSuccessor(LaneInfoConstPtr lane) const {
  if (lane->lane().successor_id().empty()) {
    return nullptr;
  }
  hdmap::Id preferred_id = lane->lane().successor_id(0);
  for (const auto &lane_id : lane->lane().successor_id()) {
    if (range_lane_ids_.count(lane_id.id()) != 0) {
      preferred_id = lane_id;
      break;
    }
  }
  return hdmap_->GetLaneById(preferred_id);
}

//求前连接的lane id
LaneInfoConstPtr PncMap::GetRoutePredecessor(LaneInfoConstPtr lane) const {
  if (lane->lane().predecessor_id().empty()) {
    return nullptr;
  }

  std::unordered_set<std::string> predecessor_ids;
  for (const auto &lane_id : lane->lane().predecessor_id()) {
    predecessor_ids.insert(lane_id.id());
  }

  hdmap::Id preferred_id = lane->lane().predecessor_id(0);
  for (size_t i = 1; i < route_indices_.size(); ++i) {
    auto &lane = route_indices_[i].segment.lane->id();
    if (predecessor_ids.count(lane.id()) != 0) {
      preferred_id = lane;
      break;
    }
  }
  return hdmap_->GetLaneById(preferred_id);
}

bool PncMap::ExtendSegments(const RouteSegments &segments,
                            const common::PointENU &point, double look_backward,
                            double look_forward,
                            RouteSegments *extended_segments) {
  common::SLPoint sl;
  LaneWaypoint waypoint;
  if (!segments.GetProjection(point, &sl, &waypoint)) {
    AERROR << "point: " << point.ShortDebugString() << " is not on segment";
    return false;
  }
  return ExtendSegments(segments, sl.s() - look_backward, sl.s() + look_forward,
                        extended_segments);
}

bool PncMap::ExtendSegments(const RouteSegments &segments, double start_s,
                            double end_s,
                            RouteSegments *const truncated_segments) const {
  //如为空，返回错误
  if (segments.empty()) {
    AERROR << "The input segments is empty";
    return false;
  }
  CHECK_NOTNULL(truncated_segments);
  //对truncated_segments的属性进行赋值，并且将segment的id传递给truncated_segments id成员
  truncated_segments->SetProperties(segments);
  //如果开始s大于结束s，则返回错误
  if (start_s >= end_s) {
    AERROR << "start_s(" << start_s << " >= end_s(" << end_s << ")";
    return false;
  }
  std::unordered_set<std::string> unique_lanes;
  static constexpr double kRouteEpsilon = 1e-3;
  // Extend the trajectory towards the start of the trajectory.
  if (start_s < 0) {
    const auto &first_segment = *segments.begin();
    auto lane = first_segment.lane;
    double s = first_segment.start_s;
    double extend_s = -start_s;
    std::vector<LaneSegment> extended_lane_segments;
    //从第一个segment开始，渐进向前查找，直到满足start_s要求的距离（需要考虑在第一个segement起始位置还是中间位置开始两种情况）
    while (extend_s > kRouteEpsilon) {
      if (s <= kRouteEpsilon) {
        lane = GetRoutePredecessor(lane);
        if (lane == nullptr ||
            unique_lanes.find(lane->id().id()) != unique_lanes.end()) {
          break;
        }
        s = lane->total_length();

      } else {
        const double length = std::min(s, extend_s);
        extended_lane_segments.emplace_back(lane, s - length, s);
        extend_s -= length;
        s -= length;
        unique_lanes.insert(lane->id().id());
      }
    }
    truncated_segments->insert(truncated_segments->begin(),
                               extended_lane_segments.rbegin(),
                               extended_lane_segments.rend());
  }
  bool found_loop = false;
  double router_s = 0;
  //segments信息更新到truncated_segments中
  for (const auto &lane_segment : segments) {
    const double adjusted_start_s = std::max(
        start_s - router_s + lane_segment.start_s, lane_segment.start_s);
    const double adjusted_end_s =
        std::min(end_s - router_s + lane_segment.start_s, lane_segment.end_s);
    if (adjusted_start_s < adjusted_end_s) {
      if (!truncated_segments->empty() &&
          truncated_segments->back().lane->id().id() ==
              lane_segment.lane->id().id()) {
        truncated_segments->back().end_s = adjusted_end_s;
      } else if (unique_lanes.find(lane_segment.lane->id().id()) ==
                 unique_lanes.end()) {
        truncated_segments->emplace_back(lane_segment.lane, adjusted_start_s,
                                         adjusted_end_s);
        unique_lanes.insert(lane_segment.lane->id().id());
      } else {
        found_loop = true;
        break;
      }
    }
    router_s += (lane_segment.end_s - lane_segment.start_s);
    if (router_s > end_s) {
      break;
    }
  }
  if (found_loop) {
    return true;
  }
  // Extend the trajectory towards the end of the trajectory.
  if (router_s < end_s && !truncated_segments->empty()) {
    auto &back = truncated_segments->back();
    if (back.lane->total_length() > back.end_s) {
      double origin_end_s = back.end_s;
      back.end_s =
          std::min(back.end_s + end_s - router_s, back.lane->total_length());
      router_s += back.end_s - origin_end_s;
    }
  }
  //获取segments中最后一个segment，从最后一个segment开始，往后进行拓展
  auto last_lane = segments.back().lane;
  while (router_s < end_s - kRouteEpsilon) {
    last_lane = GetRouteSuccessor(last_lane);
    if (last_lane == nullptr ||
        unique_lanes.find(last_lane->id().id()) != unique_lanes.end()) {
      break;
    }
    //获取每一个循环能够增加的最大长度
    const double length = std::min(end_s - router_s, last_lane->total_length());
    truncated_segments->emplace_back(last_lane, 0, length);
    unique_lanes.insert(last_lane->id().id());
    router_s += length;
  }
  return true;
}

void PncMap::AppendLaneToPoints(LaneInfoConstPtr lane, const double start_s,
                                const double end_s,
                                std::vector<MapPathPoint> *const points) {
  if (points == nullptr || start_s >= end_s) {
    return;
  }
  double accumulate_s = 0.0;
  for (size_t i = 0; i < lane->points().size(); ++i) {
    if (accumulate_s >= start_s && accumulate_s <= end_s) {
      points->emplace_back(lane->points()[i], lane->headings()[i],
                           LaneWaypoint(lane, accumulate_s));
    }
    if (i < lane->segments().size()) {
      const auto &segment = lane->segments()[i];
      const double next_accumulate_s = accumulate_s + segment.length();
      if (start_s > accumulate_s && start_s < next_accumulate_s) {
        points->emplace_back(segment.start() + segment.unit_direction() *
                                                   (start_s - accumulate_s),
                             lane->headings()[i], LaneWaypoint(lane, start_s));
      }
      if (end_s > accumulate_s && end_s < next_accumulate_s) {
        points->emplace_back(
            segment.start() + segment.unit_direction() * (end_s - accumulate_s),
            lane->headings()[i], LaneWaypoint(lane, end_s));
      }
      accumulate_s = next_accumulate_s;
    }
    if (accumulate_s > end_s) {
      break;
    }
  }
}

}  // namespace hdmap
}  // namespace apollo
