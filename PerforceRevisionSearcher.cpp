// PerforceRevisionSearcher.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "crow/crow.h"
#include <map>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <windows.h>
#include <boost/regex.hpp>


#define ENSURE( condition, doWhenFail )                                        \
if ( !(condition) )                                                            \
{                                                                              \
    printf( "[UX+] %s, Error occurred with '%s'.", __FUNCTION__, #condition ); \
    doWhenFail;                                                                \
}

#define p result.AppendLine

std::string password;
int         mode;


////////////////////////////////////////////////////////////////////////////////////////////////////
/// @class	Mode
/// 
/// @brief	mode
////////////////////////////////////////////////////////////////////////////////////////////////////
enum class Mode
{
	Search   = 0, ///< search changelists
	Download = 1, ///< download csv of changelists
	Job      = 2, ///< makes job
	Max           ///< max value of this enum
};

////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief	convert utf8 to ansi string
///
/// @param	utf8str	utf string
///
/// @return	ansi string
////////////////////////////////////////////////////////////////////////////////////////////////////
std::string utf8_to_ansi( const std::string& utf8str )
{
	int srcLen = (int)( utf8str.length() );
    int length = MultiByteToWideChar( CP_UTF8, 0, utf8str.c_str(), srcLen + 1, 0, 0 );
	if ( length <= 0 ) return "";

    std::wstring wtemp( length, (wchar_t)( 0 ) );
    MultiByteToWideChar( CP_UTF8, 0, utf8str.c_str(), srcLen + 1, &wtemp[ 0 ], length );
    length = WideCharToMultiByte( CP_ACP, 0, &wtemp[ 0 ], -1, 0, 0, 0, 0 );
    std::string temp(length, (char)( 0 ) );
    WideCharToMultiByte( CP_ACP, 0, &wtemp[ 0 ], -1, &temp[ 0 ], length, 0, 0 );
    return temp;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief	reads config
///
/// @return	success or failure
////////////////////////////////////////////////////////////////////////////////////////////////////
bool ReadConfig()
{
	FILE* configFile = fopen( "config.txt", "r" );
	if ( !configFile )
	{
		printf( "ReadConfig, configFile == nullptr\n" );
		return false;
	}

	char buf[ 1024 * 100 ];
	while ( fgets( buf, sizeof( buf ) - 1, configFile ) )
	{
		char* token = strtok( buf, " \t" );
		if ( !token ) continue;

		if ( !strcmp( token, "password" ) )
			password = strtok( nullptr, " \t\r\n" );
	}

	fclose( configFile );

	return true;
}

class Buffer
{
private:
	std::string m_result;

public:
	const std::string& GetResult() const { return m_result; }

	void Append( const char* str, ... )
	{
		va_list args;
		va_start( args, str );
		char conv[ 8192 ] = { 0, };
		vsnprintf( conv, sizeof( conv ) - 1, str, args );
		va_end( args );

		m_result += conv;
	}

	void AppendLine( const char* str, ... )
	{
		va_list args;
		va_start( args, str );
		char conv[ 8192 ] = { 0, };
		vsnprintf( conv, sizeof( conv ) - 1, str, args );
		va_end( args );

		m_result += conv;
		m_result += "\n";
	}
};


std::vector< std::string > Split( const std::string& str, const std::string& delimiters )
{
	if ( str.empty() ) return std::vector< std::string >();

	std::vector< std::string > ret;

	std::size_t index = 0;
	std::string subStr;

	for ( ; ; )
	{
		std::size_t newPos = str.find_first_of( delimiters, index );
		bool        finish = (newPos == std::string::npos);
		if ( finish )
			subStr = str.substr( index );
		else
			subStr = str.substr( index, newPos - index );

		if ( !subStr.empty() )
			ret.push_back( subStr );

		if ( finish ) break;

		index = newPos + 1;
	}

	return ret;
}

std::string Replace( const std::string& str, const std::string& oldStr, const std::string& newStr )
{
	if ( oldStr.empty() )
		return str;

	std::string result;
	std::size_t index = 0;

	while ( true )
	{
		std::size_t oldStrIndex = str.find( oldStr, index );
		if ( oldStrIndex == std::string::npos )
		{
			result += str.substr( index );
			break;
		}

		result += str.substr( index, oldStrIndex - index );
		result += newStr;
		index = oldStrIndex + oldStr.length();
	}

	return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief	searches revisions which contain a specific text
///
/// @return	no return
////////////////////////////////////////////////////////////////////////////////////////////////////
void Search(
	const std::string& depot,
	const std::string& commentRegexStr,
	const std::string& fileNameRegexStr,
	      int          startRevision,
	const std::string& startDate,
	      Mode         mode,
	const std::string& param,
	      Buffer&      result )
{
	if ( mode == Mode::Job && commentRegexStr.empty() )
	{
		p( "<script>alert( 'keyword is empty' );</script>" );
		return;
	}

	char buf[ 1024 * 100 ];
	if ( startRevision )
		sprintf_s( buf, sizeof( buf ) - 1, "p4 -C utf8 changes -t -l \"//depot/%s...@>=%d\" > log.txt", depot.c_str(), startRevision );
	else if ( !startDate.empty() )
		sprintf_s( buf, sizeof( buf ) - 1, "p4 -C utf8 changes -t -l @%s,@now //depot/%s... > log.txt", startDate.c_str(), depot.c_str() );
	else
		sprintf_s( buf, sizeof( buf ) - 1, "p4 -C utf8 changes -t -s submitted -l -m 2000 //depot/%s... > log.txt", depot.c_str() );
	system( buf );

	FILE* logFile = fopen( "log.txt", "r" );
	ENSURE( logFile, return );

	class RevisionInfo
	{
	public:
		int num;
		std::string time;
		std::string comment;
		std::list< std::string > fileList;
	};

	std::string modifiedCommentRegexStr;
	if ( !commentRegexStr.empty() )
		modifiedCommentRegexStr = ".*(" + commentRegexStr + ").*";
	else
		modifiedCommentRegexStr = commentRegexStr;

	std::string modifiedFileNameRegexStr;
	if ( !fileNameRegexStr.empty() )
		modifiedFileNameRegexStr = ".*(" + fileNameRegexStr + ").*";
	else
		modifiedFileNameRegexStr = fileNameRegexStr;

	enum class ReadMode
	{
		Normal,  ///< �Ϲ�
		Comment, ///< �ּ�
	};

	ReadMode     readMode = ReadMode::Normal;
	RevisionInfo curRevision;
	boost::regex commentReg;
	boost::regex fileNameReg;
	
	commentReg  = modifiedCommentRegexStr;
	fileNameReg = modifiedFileNameRegexStr;

	std::list< RevisionInfo > revisionList;
	
	while ( fgets( buf, sizeof( buf ) - 1, logFile ) )
	{
		switch ( readMode )
		{
		case ReadMode::Normal:
			{
				char* token = strtok( buf, " " );
				ENSURE( !strcmp( token, "Change" ), return );

				token = strtok( nullptr, " " );
				curRevision.num = atoi( token );
				token = strtok( nullptr, " " );
				token = strtok( nullptr, " " );
				curRevision.time = token;
				token = strtok( nullptr, " " );
				curRevision.time += " ";
				curRevision.time += token;
				ENSURE( fgets( buf, sizeof( buf ) - 1, logFile ), return );
				readMode = ReadMode::Comment;
				curRevision.comment = "";
			}
			break;
		case ReadMode::Comment:
			{
				if ( *buf != '\t' )
				{
					readMode = ReadMode::Normal;

					// check the revision
					if ( startRevision && curRevision.num < startRevision ) break;

					// check the date
					if ( !startDate.empty() && curRevision.time < startDate ) break;

					// check the comment
					boost::cmatch matches;
					if (
						!commentRegexStr.empty() &&
						!boost::regex_match( curRevision.comment.c_str(), matches, commentReg ) )
						break;

					revisionList.push_back( curRevision );
					break;
				}

				curRevision.comment += buf;
			}
			break;
		}
	}

	fclose( logFile );

	// When searching revisions with date, p4 doesn't filter the depot.
	// So we use only first revision number, then search again with this revision number.
	// When searching revisions with a revision number, p4 filters the depot.
	if ( !startDate.empty() && !revisionList.empty() )
	{
		const RevisionInfo& revisionInfo = revisionList.back();

		return Search(
			depot,
			commentRegexStr,
			fileNameRegexStr,
			revisionInfo.num,
			"",
			mode,
			param,
			result );
	}

// 	std::list< std::string > depotList;
// 	depotList.push_back( "//depot/" + depot );
// 
// 	while ( !depotList.empty() )
// 	{
// 		std::list< std::string > nextDepotList;
// 		
// 		for ( const std::string& eachDepot : depotList )
// 		{
// 			sprintf_s( buf, sizeof( buf ) - 1, "p4 -C utf8 fstat %s/* > log.txt", eachDepot.c_str() );
// 			system( buf );
// 
// 			FILE* logFile = fopen( "log.txt", "r" );
// 			ENSURE( logFile, return );
// 
// 			while ( fgets( buf, sizeof( buf ) - 1, logFile ) )
// 			{
// 				const char* token = strtok( buf, "\r\n" );
// 				if ( !token ) continue;
// 
// 				nextDepotList.push_back( token );
// 			}
// 
// 			fclose( logFile );
// 		}
// 
// 		depotList = nextDepotList;
// 	}

// 	int revisionIndex      = 0;
// 	int totalRevisionCount = revisionList.size();
// 	for ( RevisionInfo& revision : revisionList )
// 	{
// 		printf( "%d/%d\n", revisionIndex + 1, totalRevisionCount );
// 		revisionIndex++;
// 
// 		sprintf_s( buf, sizeof( buf ) - 1, "p4 -C utf8 describe -s %d > log.txt", revision.num );
// 		system( buf );
// 
// 		FILE* logFile = fopen( "log.txt", "r" );
// 		ENSURE( logFile, return );
// 
// 		bool affectedFilesTagFound = false;
// 		while ( fgets( buf, sizeof( buf ) - 1, logFile ) )
// 		{
// 			if ( strstr( buf, "Affected files ..." ) )
// 			{
// 				affectedFilesTagFound = true;
// 				break;
// 			}
// 		}
// 
// 		ENSURE( affectedFilesTagFound, return );
// 		ENSURE( fgets( buf, sizeof( buf ) - 1, logFile ), return );
// 
// 		while ( fgets( buf, sizeof( buf ) - 1, logFile ) )
// 		{
// 			const char* token = strtok( buf, "\r\n" );
// 			if ( !token || !*token ) break;
// 
// 			revision.fileList.push_back( token );
// 		}
// 
// 		fclose( logFile );
// 	}

	if ( mode == Mode::Search || mode == Mode::Job )
	{
		p( "<div class='contentContainer'>" );
		p( "<table class='overviewSummary' border=0 cellpadding=3 cellspacing=0 style='border-collapse:collapse; border:1px gray solid;'>" );
		p( "<caption><span>Result</span><span class=tabEnd>&nbsp;</span></caption>" );
		p( "<tr class='altColor'>" );
		p( "<th class='colFirst' scope='col'>Revision</th>" );
		p( "<th class='colOne' scope='col'>Date</th>" );
		p( "<th class='colOne' scope='col'>Comment</th>" );
//		p( "<th class='colLast' scope='col'>File</th>" );
// 		p( "<th class='colLast' scope='col'>Open</th>" );
		p( "</tr>" );

		int index = 0;
		for ( const RevisionInfo& revision : revisionList )
		{
// 			bool matched = false;
// 			for ( const std::string& fileName : revision.fileList )
// 			{
// 				boost::cmatch matches;
// 				if ( fileNameRegexStr.empty() || boost::regex_match( fileName.c_str(), matches, fileNameReg ) )
// 				{
// 					matched = true;
// 					break;
// 				}
// 			}
// 
// 			if ( !matched ) continue;

			if ( index++ % 2 )
				p( "<tr class=altColor>" );
			else
				p( "<tr class=rowColor>" );

			p( "<td class='colFirst'>%d</td>", revision.num );
			p( "<td class='colOne'>%s</td>", revision.time.c_str() );
			p( "<td class='colOne'><pre>%s</pre></td>", revision.comment.c_str() );
// 			p( "<td class='colLast'>" );
// 			for ( const std::string& fileName : revision.fileList )
// 				p( "%s<br/>", fileName.c_str() );
// 			p( "</td>" );
// 			p( "<td class='colLast'><a href='p4vc://change%%20%d'>Open</a></td>", revision.num );
			p( "</tr>" );

			if ( mode == Mode::Job )
			{
				sprintf_s(
					buf, sizeof( buf ) - 1,
					"p4 -C cp949 fix -s same -c %d \"%s\"", revision.num, utf8_to_ansi( param ).c_str() );
				system( buf );
			}
		}
		p( "</table>" );
		p( "</div>" );
	}
	else
	{
		result.Append( "\xEF\xBB\xBF" );
		p( "Revision,Date,Comment" );
		for ( const RevisionInfo& revision : revisionList )
		{
// 			bool matched = false;
// 			for ( const std::string& fileName : revision.fileList )
// 			{
// 				boost::cmatch matches;
// 				if ( boost::regex_match( fileName.c_str(), matches, fileNameReg ) )
// 				{
// 					matched = true;
// 					break;
// 				}
// 			}
// 
// 			if ( !matched ) continue;
			p(
				"%d,\"%s\",\"%s\",",
				revision.num, revision.time.c_str(), Replace( revision.comment, "\"", "\"\"" ).c_str() );
		}
	}

}

////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief	views revision
///
/// @return	no returns
////////////////////////////////////////////////////////////////////////////////////////////////////
void ViewRevision()
{
	printf( "revision: " );
	char buf[ 1024 * 100 ];
	gets_s( buf );
	int revision = atoi( buf );
	sprintf_s( buf, sizeof( buf ) - 1, "p4vc change %d", revision );
	system( buf );
}

void _ParseRequestParams( const std::string& paramPart, std::unordered_map< std::string, std::string >& params )
{
	for ( const std::string& eachParam : Split( paramPart, "&" ) )
	{
		std::vector< std::string > paramTokens = Split( eachParam, "=" );
		if ( paramTokens.empty() )
			continue;

		std::string paramName = paramTokens[ 0 ];
		std::string paramValue;
		if ( paramTokens.size() > 1 )
		{
			char buf[ 256 ];
			strcpy_s( buf, sizeof( buf ) - 1, paramTokens[ 1 ].c_str() );
			crow::qs_decode( buf );
			paramValue = buf;
		}
		
		params[ paramName ] = paramValue;
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
/// @brief	main function
///
/// @return	exit code
////////////////////////////////////////////////////////////////////////////////////////////////////
int main()
{
	if ( !ReadConfig() )
	{
		printf( "main, failed to ReadConfig\n" );
		return 1;
	}

	crow::SimpleApp app;

	CROW_ROUTE( app, "/" )
		.methods( "GET"_method, "POST"_method )
		( []( const crow::request& req, crow::response& res )
	{
		try
		{
			Buffer result;

			printf( "%s\n", req.body.c_str() );

			std::unordered_map< std::string, std::string > paramMap;
			_ParseRequestParams( req.body, paramMap );

			std::string token = paramMap[ "token" ];
			if ( !token.empty() && token != password )
				token = "";

			std::string command = paramMap[ "command" ];
			std::string depot = paramMap[ "depot" ];
			if ( depot.empty() )
				depot = "Trunk";

			std::string commentRegex  = paramMap[ "commentRegex" ];
			std::string fileNameRegex = paramMap[ "fileRegex"    ];
			std::string startDate     = paramMap[ "startDate"    ];
			std::string jobName       = paramMap[ "jobName"      ];

			int startRevision = atoi( paramMap[ "startRevision" ].c_str() );

			if ( command == "Download" )
			{
				res.add_header( "Content-Type", "application/octet-stream" );
				res.add_header( "Content-Disposition", "1; filename=result.csv" );
				Search( depot, commentRegex, fileNameRegex, startRevision, startDate, Mode::Download, "", result );
				res.write( result.GetResult() );
				res.end();
				return;
			}

			p( "<html>" );
			p( "<head>" );
			p( "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf8\">" );
			p( "<link rel='stylesheet' type='text/css' href='StyleSheet.css' title='Style'>" );
			p( "</head>" );
			p( "<body>" );
			p( "<form name=form method=post>" );
			p( "<input type='hidden' name='token' value='%s'/>", token.c_str() );
			if ( token.empty() )
			{
				p( "<script>" );
				p( "function OnLoginButtonPressed()" );
				p( "{" );
				p( "	form.submit();" );
				p( "}" );
				p( "</script>" );
				p( "Password: <input type='password' name='token' />" );
				p( "<input type='submit' value='Login' />" );
			}
			else
			{
				p( "<script>" );
				p( "function OnSearchButtonPressed()" );
				p( "{" );
				p( "	form.command.value = 'Search';" );
				p( "	form.submit();" );
				p( "}" );
				p( "function OnDownloadButtonPressed()" );
				p( "{" );
				p( "	form.command.value = 'Download';" );
				p( "	form.submit();" );
				p( "}" );
				p( "function OnJobButtonPressed()" );
				p( "{" );
				p( "	form.command.value = 'Job';" );
				p( "	form.submit();" );
				p( "}" );
				p( "</script>" );
				p( "<table>" );
				p( "<input type='hidden' name='command' />" );
				p( "<tr><td>Depot</td><td><input type='text' name='depot' value='%s' /></td><td>ex) Trunk/Server, Branches/KO/QA/Client, Branches/JP/Dev/Server</tr>", depot.c_str() );
				p( "<tr><td>Comment</td><td><input type='text' name='commentRegex' value='%s' /></td>", commentRegex.c_str() );
				p( "<td>" );
				p( "Keyword1 AND Keyword2: (?=.*Keyword1)(?=.*Keyword2)<br/>" );
				p( "Keyword1 OR Keyword2: (Keyword1)|(Keyword2)" );
				p( "</td>" );
	// 			p( "<tr><td>File</td><td><input type='text' name='fileRegex' value='%s' /></td>", fileNameRegex.c_str() );
	// 			p( "</tr>" );
				p( "<tr>" );
				p( "<td>Start Revision</td><td><input type = 'text' name = 'startRevision' value = '%d' /></td>", startRevision );
				p( "</tr>" );
				p( "<tr>" );
				p( "<td>Start Date</td><td><input type = 'text' name = 'startDate' value = '%s' /></td>", startDate.c_str() );
				p( "<td>yyyy/mm/dd ex)2017/10/29</td>" );
				p( "</tr>" );
				p( "</table>" );
				p( "<input type='submit' value='Search' onClick='OnSearchButtonPressed()' />" );
		
				if ( command == "Search" )
				{
					p( "<input type='button' value='Download' onClick='OnDownloadButtonPressed()' />" );
					p( "Job Name<input type='text' name='jobName' value='%s' /></td>", jobName.c_str() );
					p( "<input type='button' value='Job' onClick='OnJobButtonPressed()' />" );
					Search( depot, commentRegex, fileNameRegex, startRevision, startDate, Mode::Search, "", result );
				}
				else if ( command == "Job" )
				{
					p( "<input type='button' value='Download' onClick='OnDownloadButtonPressed()' />" );
					Search( depot, commentRegex, fileNameRegex, startRevision, startDate, Mode::Job, jobName, result );
				}
			}

			p( "</form>" );
			p( "<form name=downloadForm method=post action=Download.html>" );
			p( "</body>" );
			p( "</html>" );

			res.write( result.GetResult() );
			res.end();
		}
		catch ( std::exception& e )
		{
			printf( "%s\n", e.what() );
		}
	} );

	CROW_ROUTE( app, "/StyleSheet.css" )
		.methods( "GET"_method )
		( []( const crow::request& req )
		{
			std::string result;

			FILE* file = fopen( "StyleSheet.css", "r" );
			if ( !file ) return result;

			char buf[ 256 ];
			while ( fgets( buf, sizeof( buf ) - 1, file ) )
				result += buf;

			fclose( file );
			return result;
		} );

	app.port( 7777 ).run();

    return 0;
}
