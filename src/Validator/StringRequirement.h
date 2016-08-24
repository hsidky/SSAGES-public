/**
 * This file is part of
 * SSAGES - Suite for Advanced Generalized Ensemble Simulations
 *
 * Copyright 2016 Ben Sikora <bsikora906@gmail.com>
 *
 * SSAGES is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SSAGES is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SSAGES.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once 

#include <regex>
#include "Requirement.h"

namespace Json
{
	//! Requirements on strings.
	/*!
	 * \ingroup Json
	 */
	class StringRequirement : public Requirement 
	{
	private: 
		bool _minSet; //!< If \c True, minimum length requirement is active.
		bool _maxSet; //!< If \c True, maximum length requirement is active.
		bool _rgxSet; //!< If \c True, string has to match regular expression.
		size_t _minLength; //!< Minimum string length;
		size_t _maxLength; //!< Maximum string length;
		std::regex _regex; //!< Regular expression to match string to.
		std::string _expr; //!< Expression.
		std::string _path; //!< Path for JSON path specification.
		std::vector<std::string> _enum; //!< Enum values.

	public:
		//! Constructor.
		StringRequirement() : 
		_minSet(false), _maxSet(false), _rgxSet(false),
		_minLength(0), _maxLength(0), _regex(), _expr(), _path(), _enum(0)
		{}
		
		//! Reset Requirement.
		virtual void Reset() override
		{
			_minSet = false;
			_maxSet = false;
			_rgxSet = false;
			_minLength = 0;
			_maxLength = 0;
			_regex = "";
			_expr = "";
			_path  = "";
			_enum.clear();
			ClearErrors();
			ClearNotices();
		}

		//! Parse JSON value to generate Requirement.
		/*!
		 * \param json JSON input value.
		 * \param path Path for JSON path specification.
		 */
		virtual void Parse(Value json, const std::string& path) override
		{
			Reset();
			
			_path = path;
			if(json.isMember("minLength") && json["minLength"].isUInt())
			{
				_minSet = true;
				_minLength = json["minLength"].asUInt();
			}
			
			if(json.isMember("maxLength") && json["maxLength"].isUInt())
			{
				_maxSet = true;
				_maxLength = json["maxLength"].asUInt();
			
			}

			if(json.isMember("pattern") && json["pattern"].isString())
			{
				_rgxSet = true;
				_expr = json["pattern"].asString();
				_regex = std::regex(_expr, std::regex::ECMAScript);
			}

			if(json.isMember("enum") && json["enum"].isArray())
			{
				for(const auto& val : json["enum"])
					_enum.push_back(val.asString());
			}
		}

		//! Validate string value.
		/*!
		 * \param json JSON value to be validated.
		 * \param path Path for JSON path specification.
		 *
		 * This function tests if the JSON value is of type string and if the
		 * string meets the requirements loaded via StringRequirement::Parse().
		 * If the validation fails, one or more errors are appended to the list
		 * of errors.
		 */
		virtual void Validate(const Value& json, const std::string& path) override
		{
			if(!json.isString())
			{
				PushError(path + ": Must be of type \"string\"");
				return;
			}
			
			if(_minSet && json.asString().length() < _minLength)
				PushError(path + ": Length must be greater than " + std::to_string(_minLength));
			
			if(_maxSet && json.asString().length() > _maxLength)
				PushError(path + ": Length must be less than " + std::to_string(_minLength));

			if(_rgxSet && !std::regex_match(json.asString(), _regex))
				PushError(path + ": String must match regular expression \"" + _expr + "\"");

			if(_enum.size() && std::find(_enum.begin(),_enum.end(), json.asString()) == _enum.end())
				PushError(path + ": String is not a valid entry");
		}
	};
}