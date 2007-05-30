//
// StatementImpl.cpp
//
// $Id: //poco/Main/Data/src/StatementImpl.cpp#19 $
//
// Library: Data
// Package: DataCore
// Module:  StatementImpl
//
// Copyright (c) 2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
// 
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//


#include "Poco/Data/StatementImpl.h"
#include "Poco/Data/SessionImpl.h"
#include "Poco/Data/DataException.h"
#include "Poco/Data/AbstractBinder.h"
#include "Poco/Data/Extraction.h"
#include "Poco/Data/BLOB.h"
#include "Poco/SharedPtr.h"
#include "Poco/String.h"
#include "Poco/Exception.h"


using Poco::icompare;


namespace Poco {
namespace Data {


const std::string StatementImpl::VECTOR = "vector";
const std::string StatementImpl::LIST = "list";
const std::string StatementImpl::DEQUE = "deque";
const std::string StatementImpl::UNKNOWN = "unknown";


StatementImpl::StatementImpl(SessionImpl& rSession):
	_state(ST_INITIALIZED),
	_extrLimit(upperLimit((Poco::UInt32) Limit::LIMIT_UNLIMITED, false)),
	_lowerLimit(0),
	_columnsExtracted(0),
	_rSession(rSession),
	_storage(STORAGE_UNKNOWN_IMPL),
	_ostr(),
	_bindings()
{
}


StatementImpl::~StatementImpl()
{
}


Poco::UInt32 StatementImpl::execute()
{
	resetExtraction();
	Poco::UInt32 lim = 0;
	if (_lowerLimit > _extrLimit.value())
		throw LimitException("Illegal Statement state. Upper limit must not be smaller than the lower limit.");

	if (_extrLimit.value() == Limit::LIMIT_UNLIMITED)
		lim = executeWithoutLimit();
	else
		lim = executeWithLimit();
		
	if (lim < _lowerLimit)
	{
		throw LimitException("Did not receive enough data.");
	}
	return lim;
}


Poco::UInt32 StatementImpl::executeWithLimit()
{
	poco_assert (_state != ST_DONE);

	compile();

	Poco::UInt32 count = 0;
	do
	{
		bind();
		while (hasNext() && count < _extrLimit.value())
		{
			next();
			++count;
		}
	}
	while (canBind());

	if (!canBind() && (!hasNext() || _extrLimit.value() == 0))
		_state = ST_DONE;
	else if (hasNext() && _extrLimit.value() == count && _extrLimit.isHardLimit())
		throw LimitException("HardLimit reached. We got more data than we asked for");

	return count;
}


Poco::UInt32 StatementImpl::executeWithoutLimit()
{
	poco_assert (_state != ST_DONE);
	
	compile();

	Poco::UInt32 count = 0;
	do
	{
		bind();
		while (hasNext())
		{
			next();
			++count;
		}
	}
	while (canBind());

	_state = ST_DONE;
	return count;
}


void StatementImpl::compile()
{
	if (_state == ST_INITIALIZED)
	{
		compileImpl();
		_state = ST_COMPILED;

		if (!extractions().size())
		{
			Poco::UInt32 cols = columnsReturned();
			if (cols) makeExtractors(cols);
		}

		fixupExtraction();
		fixupBinding();
	}
	else if (_state == ST_RESET)
	{
		resetBinding();
		resetExtraction();
		_state = ST_COMPILED;
	}
}


void StatementImpl::bind()
{
	if (_state == ST_COMPILED)
	{
		bindImpl();
		_state = ST_BOUND;
	}
	else if (_state == ST_BOUND)
	{
		if (!hasNext())
		{
			if (canBind())
			{
				bindImpl();
			}
			else
				_state = ST_DONE;
		}
	}
}


void StatementImpl::reset()
{
	_state = ST_RESET;
	compile();
}


void StatementImpl::setExtractionLimit(const Limit& extrLimit)
{
	if (!extrLimit.isLowerLimit())
		_extrLimit = extrLimit;
	else
		_lowerLimit = extrLimit.value();
}


void StatementImpl::fixupExtraction()
{
	Poco::Data::AbstractExtractionVec::iterator it    = extractions().begin();
	Poco::Data::AbstractExtractionVec::iterator itEnd = extractions().end();
	AbstractExtractor& ex = extractor();
	_columnsExtracted = 0;
	for (; it != itEnd; ++it)
	{
		(*it)->setExtractor(&ex);
		(*it)->setLimit(_extrLimit.value()),
		_columnsExtracted += (int)(*it)->numOfColumnsHandled();
	}
}


void StatementImpl::fixupBinding()
{
	// no need to call binder().reset(); here will be called before each bind anyway
	AbstractBindingVec::iterator it    = bindings().begin();
	AbstractBindingVec::iterator itEnd = bindings().end();
	AbstractBinder& bin = binder();
	std::size_t numRows = 0;
	if (it != itEnd)
		numRows = (*it)->numOfRowsHandled();
	for (; it != itEnd; ++it)
	{
		if (numRows != (*it)->numOfRowsHandled())
		{
			throw BindingException("Size mismatch in Bindings. All Bindings MUST have the same size");
		}
		(*it)->setBinder(&bin);
	}
}


void StatementImpl::resetBinding()
{
	AbstractBindingVec::iterator it    = bindings().begin();
	AbstractBindingVec::iterator itEnd = bindings().end();
	for (; it != itEnd; ++it)
	{
		(*it)->reset();
	}
}


void StatementImpl::resetExtraction()
{
	Poco::Data::AbstractExtractionVec::iterator it = extractions().begin();
	Poco::Data::AbstractExtractionVec::iterator itEnd = extractions().end();
	for (; it != itEnd; ++it)
	{
		(*it)->reset();
	}
}


void StatementImpl::setStorage(const std::string& storage)
{
	if (0 == icompare(VECTOR, storage))
		_storage = STORAGE_VECTOR_IMPL; 
	else if (0 == icompare(LIST, storage))
		_storage = STORAGE_LIST_IMPL;
	else if (0 == icompare(DEQUE, storage))
		_storage = STORAGE_DEQUE_IMPL;
	else if (0 == icompare(UNKNOWN, storage))
		_storage = STORAGE_UNKNOWN_IMPL;
	else
		throw NotFoundException();
}


void StatementImpl::makeExtractors(Poco::UInt32 count)
{
	std::string storage;
	
	switch (_storage)
	{
	case STORAGE_VECTOR_IMPL: storage = VECTOR; break;
	case STORAGE_LIST_IMPL:   storage = LIST; break;
	case STORAGE_DEQUE_IMPL:  storage = DEQUE; break;
	case STORAGE_UNKNOWN_IMPL:
		storage = AnyCast<std::string>(session().getProperty("storage")); 
		break;
	}

	if ("" == storage) storage = VECTOR;

	for (int i = 0; i < count; ++i)
	{
		const MetaColumn& mc = metaColumn(i);
		switch (mc.type())
		{
			case MetaColumn::FDT_BOOL:
			case MetaColumn::FDT_INT8:  
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<Int8, std::vector<Int8> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<Int8, std::list<Int8> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<Int8, std::deque<Int8> >(mc);
				break;
			case MetaColumn::FDT_UINT8:  
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<UInt8, std::vector<UInt8> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<UInt8, std::list<UInt8> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<UInt8, std::deque<UInt8> >(mc);
				break;
			case MetaColumn::FDT_INT16:  
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<Int16, std::vector<Int16> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<Int16, std::list<Int16> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<Int16, std::deque<Int16> >(mc);
				break;
			case MetaColumn::FDT_UINT16: 
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<UInt16, std::vector<UInt16> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<UInt16, std::list<UInt16> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<UInt16, std::deque<UInt16> >(mc);
				break;
			case MetaColumn::FDT_INT32:  
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<Int32, std::vector<Int32> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<Int32, std::list<Int32> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<Int32, std::deque<Int32> >(mc);
				break;
			case MetaColumn::FDT_UINT32: 
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<UInt32, std::vector<UInt32> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<UInt32, std::list<UInt32> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<UInt32, std::deque<UInt32> >(mc);
				break;
			case MetaColumn::FDT_INT64:  
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<Int64, std::vector<Int64> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<Int64, std::list<Int64> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<Int64, std::deque<Int64> >(mc); 
				break;
			case MetaColumn::FDT_UINT64: 
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<UInt64, std::vector<UInt64> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<UInt64, std::list<UInt64> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<UInt64, std::deque<UInt64> >(mc);
				break;
			case MetaColumn::FDT_FLOAT:  
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<float, std::vector<float> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<float, std::list<float> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<float, std::deque<float> >(mc);
				break;
			case MetaColumn::FDT_DOUBLE: 
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<double, std::vector<double> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<double, std::list<double> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<double, std::deque<double> >(mc); 
				break;
			case MetaColumn::FDT_STRING: 
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<std::string, std::vector<std::string> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<std::string, std::list<std::string> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<std::string, std::deque<std::string> >(mc);
				break;
			case MetaColumn::FDT_BLOB:   
				if (0 == icompare(VECTOR, storage))
					addInternalExtract<BLOB, std::vector<BLOB> >(mc); 
				else if (0 == icompare(LIST, storage))
					addInternalExtract<BLOB, std::list<BLOB> >(mc);
				else if (0 == icompare(DEQUE, storage))
					addInternalExtract<BLOB, std::deque<BLOB> >(mc);
				break;
			default:
				throw Poco::InvalidArgumentException("Data type not supported.");
		}
	}
}


const MetaColumn& StatementImpl::metaColumn(const std::string& name) const
{
	Poco::UInt32 cols = columnsReturned();
	for (Poco::UInt32 i = 0; i < cols; ++i)
	{
		const MetaColumn& column = metaColumn(i);
		if (0 == icompare(column.name(), name)) return column;
	}

	throw NotFoundException(format("Invalid column name: %s", name));
}


} } // namespace Poco::Data
