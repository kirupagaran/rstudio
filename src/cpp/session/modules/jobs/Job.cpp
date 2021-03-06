/*
 * SessionJob.cpp
 *
 * Copyright (C) 2009-18 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <ctime>

#include <boost/make_shared.hpp>
#include <core/json/JsonRpc.hpp>

#include <session/SessionModuleContext.hpp>

#include "Job.hpp"

#define kJobId          "id"
#define kJobName        "name"
#define kJobStatus      "status"
#define kJobProgress    "progress"
#define kJobMax         "max"
#define kJobState       "state"
#define kJobRecorded    "recorded"
#define kJobStarted     "started"
#define kJobCompleted   "completed"
#define kJobElapsed     "elapsed"

#define kJobStateIdle      "idle"
#define kJobStateRunning   "running"
#define kJobStateSucceeded "succeeded"
#define kJobStateCancelled "cancelled"
#define kJobStateFailed    "failed"

using namespace rstudio::core;

namespace rstudio {
namespace session {
namespace modules { 
namespace jobs {

Job::Job(const std::string& id, 
         const std::string& name,
         const std::string& status,
         const std::string& group,
         int progress, 
         int max,
         JobState state,
         bool autoRemove):
   id_(id), 
   name_(name),
   status_(status),
   group_(group),
   state_(JobIdle),
   progress_(progress),
   max_(max),
   recorded_(::time(0)),
   started_(0),
   completed_(0),
   autoRemove_(autoRemove),
   listening_(false)
{
   setState(state);
}

Job::Job():
   state_(JobIdle),
   progress_(0),
   max_(0),
   recorded_(::time(0)),
   started_(0),
   completed_(0),
   autoRemove_(true),
   listening_(false)
{
}

std::string Job::id() const
{
    return id_;
}

std::string Job::name() const
{
    return name_;
}

std::string Job::status() const
{
    return status_;
}

std::string Job::group() const
{
    return group_;
}

int Job::progress() const
{
    return progress_;
}

int Job::max() const
{
    return max_;
}

JobState Job::state() const
{
    return state_; 
}

json::Object Job::toJson() const
{
   json::Object job;

   // fill out fields from local information
   job[kJobId]         = id_;
   job[kJobName]       = name_;
   job[kJobStatus]     = status_;
   job[kJobProgress]   = progress_;
   job[kJobMax]        = max_;
   job[kJobState]      = static_cast<int>(state_);
   job[kJobRecorded]   = static_cast<int64_t>(recorded_);
   job[kJobStarted]    = static_cast<int64_t>(started_);
   job[kJobCompleted]  = static_cast<int64_t>(completed_);

   // amend with computed elapsed time
   if (started_ >= recorded_ && started_ > completed_)
   {
      // job is started but not finished; emit the running time
      job[kJobElapsed] = static_cast<int64_t>(::time(0) - started_);
   }
   else if (completed_ > started_)
   {
      // job is completed; emit the total time spent running it
      job[kJobElapsed] = static_cast<int64_t>(completed_ - started_);
   }
   else
   {
      // job hasn't even been started yet so hasn't spent any time running
      job[kJobElapsed] = 0;
   }

   // amend running state description
   job["state_description"] = stateAsString(state_);

   return job;
}

Error Job::fromJson(const json::Object& src, boost::shared_ptr<Job> *pJobOut)
{
   boost::shared_ptr<Job> pJob = boost::make_shared<Job>();
   int state = static_cast<int>(JobIdle);
   boost::int64_t recorded = 0, started = 0, completed = 0;
   Error error = json::readObject(src,
      kJobId,        &pJob->id_,
      kJobName,      &pJob->name_,
      kJobStatus,    &pJob->status_,
      kJobProgress,  &pJob->progress_,
      kJobMax,       &pJob->max_,
      kJobRecorded,  &recorded,
      kJobStarted,   &started,
      kJobCompleted, &completed,
      kJobState,     &state);
   if (error)
      return error;

   // convert to types that aren't JSON friendly
   pJob->state_     = static_cast<JobState>(state);
   pJob->recorded_  = static_cast<time_t>(recorded);
   pJob->started_   = static_cast<time_t>(started);
   pJob->completed_ = static_cast<time_t>(started);

   *pJobOut = pJob;
   return Success();
}

void Job::setProgress(int units)
{
   // ensure units doesn't exceed the max
   if (units > max())
      progress_ = max_;
   else
      progress_ = units;

   // update job state automatically
   if (state_ == JobIdle && progress_ > 0 && progress_ < max_)
      setState(JobRunning);
   else if (state_ == JobRunning && progress_ == max_)
      setState(JobSucceeded);
}

void Job::setProgressMax(int max)
{
   max_ = max;
}

void Job::setStatus(const std::string& status)
{
   status_ = status;
}

void Job::setState(JobState state)
{
   // ignore if state doesn't change
   if (state_ == state)
      return;

   // if transitioning away from idle, set start time
   if (state_ == JobIdle && state != JobIdle)
      started_ = ::time(0);

   // record new state
   state_ = state;

   // if transitioned to a complete state, save finished time
   if (complete())
      completed_ = ::time(0);
}

void Job::setListening(bool listening)
{
   listening_ = listening;
}

bool Job::complete() const
{
   return state_ != JobIdle && state_ != JobRunning;
}

bool Job::autoRemove() const
{
   return autoRemove_;
}

time_t Job::recorded() const
{
   return recorded_;
}

time_t Job::started() const
{
   return started_;
}

time_t Job::completed() const
{
   return completed_;
}

FilePath Job::jobCacheFolder()
{
   return module_context::sessionScratchPath().complete("jobs");
}

FilePath Job::outputCacheFile()
{
   return jobCacheFolder().complete(id_ + "-output.json");
}

void Job::addOutput(const std::string& output, bool asError)
{
   // look up output file
   Error error;
   FilePath outputFile = outputCacheFile();
   int type = asError ? 
            module_context::kCompileOutputError : 
            module_context::kCompileOutputNormal;

   // let the client know, if the client happens to be listening (the client doesn't listen by
   // default because listening to all jobs simultaneously could produce an overwhelming number of
   // client events)
   if (listening_)
   {
      json::Array data;
      data.push_back(id_);
      data.push_back(type);
      data.push_back(output);
      module_context::enqueClientEvent(
            ClientEvent(client_events::kJobOutput, data));
   }

   // create parent folder if necessary
   if (!outputFile.parent().exists())
   {
      error = outputFile.parent().ensureDirectory();
      if (error)
      {
         LOG_ERROR(error);
         return;
      }
   }

   // open output file for writing
   boost::shared_ptr<std::ostream> file;
   error = outputFile.open_w(&file, false /* don't truncate */);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   // create json array with output and write it to the file
   json::Array contents;
   contents.push_back(type);
   contents.push_back(output);
   json::write(contents, *file);

   // append a newline (the file is newline-delimited JSON)
   *file << std::endl;
}

json::Array Job::output(int position)
{
   // read the lines from the file
   json::Array output;
   FilePath outputFile = outputCacheFile();
   boost::shared_ptr<std::istream> pIfs;
   Error error = outputFile.open_r(&pIfs);
   if (error)
   {
      // path not found is expected if the job hasn't produced any output yet
      if (!isPathNotFoundError(error))
         LOG_ERROR(error);
      return json::Array();
   }

   try
   {
      int line = 0;
      std::string content;
      json::Value val;

      // reading eof can trigger a failbit
      pIfs->exceptions(std::istream::badbit);

      // read each line; parse it as JSON and add it to the output array if it's past the sought
      // position
      while (!pIfs->eof())
      {
         std::getline(*pIfs, content);
         if (++line > position)
         {
            if (json::parse(content, &val))
            {
               output.push_back(val);
            }
         }
      }
   }
   catch(const std::exception& e)
   {
      error = systemError(boost::system::errc::io_error, 
                                ERROR_LOCATION);
      error.addProperty("what", e.what());
      error.addProperty("path", outputFile.absolutePath());
      LOG_ERROR(error);
   }

   return output;
}

void Job::cleanup()
{
   outputCacheFile().removeIfExists();
}

std::string Job::stateAsString(JobState state)
{
   switch(state)
   {
      case JobIdle:      return kJobStateIdle;
      case JobRunning:   return kJobStateRunning;
      case JobSucceeded: return kJobStateSucceeded;
      case JobCancelled: return kJobStateCancelled;
      case JobFailed:    return kJobStateFailed;
      case JobInvalid:   return "";
   }
   return "";
}

JobState Job::stringAsState(const std::string& state)
{
   if (state == kJobStateIdle)            return JobIdle;
   else if (state == kJobStateRunning)    return JobRunning;
   else if (state == kJobStateSucceeded)  return JobSucceeded;
   else if (state == kJobStateCancelled)  return JobCancelled;
   else if (state == kJobStateFailed)     return JobFailed;

   return JobInvalid;
}

} // namepsace jobs
} // namespace modules
} // namespace session
} // namespace rstudio

