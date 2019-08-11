//    Copyright (C) 2018-2019 Jakub Melka
//
//    This file is part of PdfForQt.
//
//    PdfForQt is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Lesser General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    PdfForQt is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public License
//    along with PDFForQt.  If not, see <https://www.gnu.org/licenses/>.


#include "pdfdocumentreader.h"
#include "pdfconstants.h"
#include "pdfxreftable.h"
#include "pdfexception.h"
#include "pdfparser.h"
#include "pdfstreamfilters.h"

#include <QFile>

#include <regex>
#include <cctype>
#include <algorithm>
#include <execution>

namespace pdf
{

PDFDocumentReader::PDFDocumentReader(const std::function<QString(bool*)>& getPasswordCallback) :
    m_successfull(true),
    m_getPasswordCallback(getPasswordCallback)
{

}

PDFDocument PDFDocumentReader::readFromFile(const QString& fileName)
{
    QFile file(fileName);

    reset();

    if (file.exists())
    {
        if (file.open(QFile::ReadOnly))
        {
            PDFDocument document = readFromDevice(&file);
            file.close();
            return document;
        }
        else
        {
            m_successfull = false;
            m_errorMessage = tr("File '%1' cannot be opened for reading. %1").arg(file.errorString());
        }
    }
    else
    {
        m_successfull = false;
        m_errorMessage = tr("File '%1' doesn't exist.").arg(fileName);
    }

    return PDFDocument();
}

PDFDocument PDFDocumentReader::readFromDevice(QIODevice* device)
{
    reset();

    if (device->isOpen())
    {
        if (device->isReadable())
        {
            // Do not close the device, it was not opened by us.
            return readFromBuffer(device->readAll());
        }
        else
        {
            m_successfull = false;
            m_errorMessage = tr("Device is not opened for reading.");
        }
    }
    else if (device->open(QIODevice::ReadOnly))
    {
        QByteArray byteArray = device->readAll();
        device->close();
        return readFromBuffer(byteArray);
    }
    else
    {
        m_successfull = false;
        m_errorMessage = tr("Can't open device for reading.");
    }

    return PDFDocument();
}

PDFDocument PDFDocumentReader::readFromBuffer(const QByteArray& buffer)
{
    try
    {
        // FOOTER CHECKING
        //  1) Check, if EOF marking is present
        //  2) Find start of cross reference table
        if (findFromEnd(PDF_END_OF_FILE_MARK, buffer, PDF_FOOTER_SCAN_LIMIT) == FIND_NOT_FOUND_RESULT)
        {
            throw PDFParserException(tr("End of file marking was not found."));
        }

        const int startXRefPosition = findFromEnd(PDF_START_OF_XREF_MARK, buffer, PDF_FOOTER_SCAN_LIMIT);
        if (startXRefPosition == FIND_NOT_FOUND_RESULT)
        {
            throw PDFParserException(tr("Start of object reference table not found."));
        }

        Q_ASSERT(startXRefPosition + std::strlen(PDF_START_OF_XREF_MARK) < buffer.size());
        PDFLexicalAnalyzer analyzer(buffer.constData() + startXRefPosition + std::strlen(PDF_START_OF_XREF_MARK), buffer.constData() + buffer.size());
        const PDFLexicalAnalyzer::Token token = analyzer.fetch();
        if (token.type != PDFLexicalAnalyzer::TokenType::Integer)
        {
            throw PDFParserException(tr("Start of object reference table not found."));
        }
        const PDFInteger firstXrefTableOffset = token.data.toLongLong();

        // HEADER CHECKING
        //  1) Check if header is present
        //  2) Scan header version

        // According to PDF Reference 1.7, Appendix H, file header can have two formats:
        //  - %PDF-x.x
        //  - %!PS-Adobe-y.y PDF-x.x
        // We will search for both of these formats.

        std::regex headerRegExp(PDF_FILE_HEADER_REGEXP);
        std::cmatch headerMatch;

        auto itBegin = buffer.cbegin();
        auto itEnd = std::next(buffer.cbegin(), qMin(buffer.size(), PDF_HEADER_SCAN_LIMIT));

        if (std::regex_search(itBegin, itEnd, headerMatch, headerRegExp))
        {
            // Size depends on regular expression, not on the text (if regular expresion is matched)
            Q_ASSERT(headerMatch.size() == 3);
            Q_ASSERT(headerMatch[1].matched != headerMatch[2].matched);

            for (int i : { 1, 2 })
            {
                if (headerMatch[i].matched)
                {
                    Q_ASSERT(std::distance(headerMatch[i].first, headerMatch[i].second) == 3);
                    m_version = PDFVersion(*headerMatch[i].first - '0', *std::prev(headerMatch[i].second) - '0');
                    break;
                }
            }
        }
        else
        {
            throw PDFParserException(tr("Header of PDF file was not found."));
        }

        // Check, if version is valid
        if (!m_version.isValid())
        {
            throw PDFParserException(tr("Version of the PDF file is not valid."));
        }

        // Now, we are ready to scan xref table
        PDFXRefTable xrefTable;
        xrefTable.readXRefTable(nullptr, buffer, firstXrefTableOffset);

        // This lambda function fetches object from the buffer from the specified offset.
        // Can throw exception, returns a pair of scanned reference and object content.
        auto getObject = [&buffer](PDFParsingContext* context, PDFInteger offset, PDFObjectReference reference) -> PDFObject
        {
            PDFParsingContext::PDFParsingContextGuard guard(context, reference);

            PDFParser parser(buffer, context, PDFParser::AllowStreams);
            parser.seek(offset);

            PDFObject objectNumber = parser.getObject();
            PDFObject generation = parser.getObject();

            if (!objectNumber.isInt() || !generation.isInt())
            {
                throw PDFParserException(tr("Can't read object at position %1.").arg(offset));
            }

            if (!parser.fetchCommand(PDF_OBJECT_START_MARK))
            {
                throw PDFParserException(tr("Can't read object at position %1.").arg(offset));
            }

            PDFObject object = parser.getObject();

            if (!parser.fetchCommand(PDF_OBJECT_END_MARK))
            {
                throw PDFParserException(tr("Can't read object at position %1.").arg(offset));
            }

            PDFObjectReference scannedReference(objectNumber.getInteger(), generation.getInteger());
            if (scannedReference != reference)
            {
                throw PDFParserException(tr("Can't read object at position %1.").arg(offset));
            }

            return object;
        };

        auto objectFetcher = [&getObject, &xrefTable](PDFParsingContext* context, PDFObjectReference reference) -> PDFObject
        {
            const PDFXRefTable::Entry& entry = xrefTable.getEntry(reference);
            switch (entry.type)
            {
                case PDFXRefTable::EntryType::Free:
                    return PDFObject();

                case PDFXRefTable::EntryType::Occupied:
                {
                    Q_ASSERT(entry.reference == reference);
                    return getObject(context, entry.offset, reference);
                }

                default:
                {
                    Q_ASSERT(false);
                    break;
                }
            }

            return PDFObject();
        };

        PDFObjectStorage::PDFObjects objects;
        objects.resize(xrefTable.getSize());

        std::vector<PDFXRefTable::Entry> occupiedEntries = xrefTable.getOccupiedEntries();

        // First, process regular objects
        auto processEntry = [this, &getObject, &objectFetcher, &objects](const PDFXRefTable::Entry& entry)
        {
            Q_ASSERT(entry.type == PDFXRefTable::EntryType::Occupied);

            if (m_successfull)
            {
                try
                {
                    PDFParsingContext context(objectFetcher);
                    PDFObject object = getObject(&context, entry.offset, entry.reference);

                    QMutexLocker lock(&m_mutex);
                    objects[entry.reference.objectNumber] = PDFObjectStorage::Entry(entry.reference.generation, object);
                }
                catch (PDFParserException exception)
                {
                    QMutexLocker lock(&m_mutex);
                    m_successfull = false;
                    m_errorMessage = exception.getMessage();
                }
            }
        };

        // Now, we are ready to scan all objects
        std::for_each(std::execution::parallel_policy(), occupiedEntries.cbegin(), occupiedEntries.cend(), processEntry);

        // ------------------------------------------------------------------------------------------
        //    SECURITY - handle encrypted documents
        // ------------------------------------------------------------------------------------------
        const PDFObject& trailerDictionaryObject = xrefTable.getTrailerDictionary();

        const PDFDictionary* trailerDictionary = nullptr;
        if (trailerDictionaryObject.isDictionary())
        {
            trailerDictionary = trailerDictionaryObject.getDictionary();
        }
        else if (trailerDictionaryObject.isStream())
        {
            const PDFStream* stream = trailerDictionaryObject.getStream();
            trailerDictionary = stream->getDictionary();
        }
        else
        {
            throw PDFParserException(tr("Invalid trailer dictionary."));
        }

        // Read the document ID
        QByteArray id;
        const PDFObject& idArrayObject = trailerDictionary->get("ID");
        if (idArrayObject.isArray())
        {
            const PDFArray* idArray = idArrayObject.getArray();
            if (idArray->getCount() > 0)
            {
                const PDFObject& idArrayItem = idArray->getItem(0);
                if (idArrayItem.isString())
                {
                    id = idArrayItem.getString();
                }
            }
        }

        PDFObject encryptObject = trailerDictionary->get("Encrypt");
        if (encryptObject.isReference())
        {
            PDFObjectReference encryptObjectReference = encryptObject.getReference();
            if (static_cast<size_t>(encryptObjectReference.objectNumber) < objects.size() && objects[encryptObjectReference.objectNumber].generation == encryptObjectReference.generation)
            {
                encryptObject = objects[encryptObjectReference.objectNumber].object;
            }
        }

        // Read the security handler
        PDFSecurityHandlerPointer securityHandler = PDFSecurityHandler::createSecurityHandler(encryptObject, id);
        PDFSecurityHandler::AuthorizationResult authorizationResult = securityHandler->authenticate(m_getPasswordCallback);

        if (authorizationResult == PDFSecurityHandler::AuthorizationResult::Failed ||
            authorizationResult == PDFSecurityHandler::AuthorizationResult::Cancelled)
        {
            // TODO: If user cancels it, do not display error message.
            throw PDFParserException(PDFTranslationContext::tr("Authorization failed. Bad password provided."));
        }

        // ------------------------------------------------------------------------------------------
        //    SECURITY - security handler created
        // ------------------------------------------------------------------------------------------

        // Then process object streams
        std::vector<PDFXRefTable::Entry> objectStreamEntries = xrefTable.getObjectStreamEntries();
        std::set<PDFObjectReference> objectStreams;
        for (const PDFXRefTable::Entry& entry : objectStreamEntries)
        {
            Q_ASSERT(entry.type == PDFXRefTable::EntryType::InObjectStream);
            objectStreams.insert(entry.objectStream);
        }

        auto processObjectStream = [this, &getObject, &objectFetcher, &objects, &objectStreamEntries] (const PDFObjectReference& objectStreamReference)
        {
            if (!m_successfull)
            {
                return;
            }

            try
            {
                PDFParsingContext context(objectFetcher);
                if (objectStreamReference.objectNumber >= static_cast<PDFInteger>(objects.size()))
                {
                    throw PDFParserException(PDFTranslationContext::tr("Object stream %1 not found.").arg(objectStreamReference.objectNumber));
                }

                const PDFObject& object = objects[objectStreamReference.objectNumber].object;
                if (!object.isStream())
                {
                    throw PDFParserException(PDFTranslationContext::tr("Object stream %1 is invalid.").arg(objectStreamReference.objectNumber));
                }

                const PDFStream* objectStream = object.getStream();
                const PDFDictionary* objectStreamDictionary = objectStream->getDictionary();

                const PDFObject& objectStreamType = objectStreamDictionary->get("Type");
                if (!objectStreamType.isName() || objectStreamType.getString() != "ObjStm")
                {
                    throw PDFParserException(PDFTranslationContext::tr("Object stream %1 is invalid.").arg(objectStreamReference.objectNumber));
                }

                const PDFObject& nObject = objectStreamDictionary->get("N");
                const PDFObject& firstObject = objectStreamDictionary->get("First");
                if (!nObject.isInt() || !firstObject.isInt())
                {
                    throw PDFParserException(PDFTranslationContext::tr("Object stream %1 is invalid.").arg(objectStreamReference.objectNumber));
                }

                // Number of objects in object stream dictionary
                const PDFInteger n = nObject.getInteger();
                const PDFInteger first = firstObject.getInteger();

                QByteArray objectStreamData = PDFStreamFilterStorage::getDecodedStream(objectStream);

                PDFParsingContext::PDFParsingContextGuard guard(&context, objectStreamReference);
                PDFParser parser(objectStreamData, &context, PDFParser::AllowStreams);

                std::vector<std::pair<PDFInteger, PDFInteger>> objectNumberAndOffset;
                objectNumberAndOffset.reserve(n);
                for (PDFInteger i = 0; i < n; ++i)
                {
                    PDFObject currentObjectNumber = parser.getObject();
                    PDFObject currentOffset = parser.getObject();

                    if (!currentObjectNumber.isInt() || !currentOffset.isInt())
                    {
                        throw PDFParserException(PDFTranslationContext::tr("Object stream %1 is invalid.").arg(objectStreamReference.objectNumber));
                    }

                    const PDFInteger objectNumber = currentObjectNumber.getInteger();
                    const PDFInteger offset = currentOffset.getInteger() + first;
                    objectNumberAndOffset.emplace_back(objectNumber, offset);
                }

                for (size_t i = 0; i < objectNumberAndOffset.size(); ++i)
                {
                    const PDFInteger objectNumber = objectNumberAndOffset[i].first;
                    const PDFInteger offset = objectNumberAndOffset[i].second;
                    parser.seek(offset);

                    PDFObject object = parser.getObject();
                    auto predicate = [objectNumber, objectStreamReference](const PDFXRefTable::Entry& entry) -> bool { return entry.reference.objectNumber == objectNumber && entry.objectStream == objectStreamReference; };
                    if (std::find_if(objectStreamEntries.cbegin(), objectStreamEntries.cend(), predicate) != objectStreamEntries.cend())
                    {
                        QMutexLocker lock(&m_mutex);
                        objects[objectNumber].object = qMove(object);
                    }
                    else
                    {
                        throw PDFParserException(PDFTranslationContext::tr("Object stream %1 is invalid.").arg(objectStreamReference.objectNumber));
                    }
                }
            }
            catch (PDFParserException exception)
            {
                QMutexLocker lock(&m_mutex);
                m_successfull = false;
                m_errorMessage = exception.getMessage();
            }
        };

        // Now, we are ready to scan all object streams
        std::for_each(std::execution::parallel_policy(), objectStreams.cbegin(), objectStreams.cend(), processObjectStream);

        PDFObjectStorage storage(std::move(objects), PDFObject(xrefTable.getTrailerDictionary()), std::move(securityHandler));
        return PDFDocument(std::move(storage));
    }
    catch (PDFParserException parserException)
    {
        m_successfull = false;
        m_errorMessage = parserException.getMessage();
    }

    return PDFDocument();
}

void PDFDocumentReader::reset()
{
    m_successfull = true;
    m_errorMessage = QString();
    m_version = PDFVersion();
}

int PDFDocumentReader::findFromEnd(const char* what, const QByteArray& byteArray, int limit)
{
    if (byteArray.isEmpty())
    {
        // Byte array is empty, no value found
        return FIND_NOT_FOUND_RESULT;
    }

    const int size = byteArray.size();
    const int adjustedLimit = qMin(byteArray.size(), limit);
    const int whatLength = static_cast<int>(std::strlen(what));

    if (adjustedLimit < whatLength)
    {
        // Buffer is smaller than scan string
        return FIND_NOT_FOUND_RESULT;
    }

    auto itBegin = std::next(byteArray.cbegin(), size - adjustedLimit);
    auto itEnd = byteArray.cend();
    auto it = std::find_end(itBegin, itEnd, what, std::next(what, whatLength));

    if (it != byteArray.cend())
    {
        return std::distance(byteArray.cbegin(), it);
    }

    return FIND_NOT_FOUND_RESULT;
}

}   // namespace pdf
