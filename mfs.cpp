// The MIT License (MIT)
// 
// Copyright (c) 2016, 2017 Trevor Bakker 
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

//#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <vector>
#include <string>
#include <iostream>
#include <stdint.h>
#include <sstream>

#define WHITESPACE " \t\n"      // We want to split our command line up into tokens
	                            // so we need to define what delimits our tokens.
	                            // In this case  white space
	                            // will separate the tokens on our command line

#define MAX_COMMAND_SIZE 255    // The maximum command-line size

#define MAX_NUM_ARGUMENTS 5     // Mav shell only supports five arguments

struct __attribute__((__packed__)) DirectoryEntry
{
	char DIR_NAME[11];
	uint8_t DIR_Attr;
	uint8_t Unused1[8];
	uint16_t DIR_FirstClusterHigh;
	uint8_t Unused2[4];
	uint16_t DIR_FirstClusterLow;
	uint32_t DIR_FileSize;
} typedef dirEntry;

using namespace std;

FILE *fp = NULL;
char BS_OEMName[8];
int16_t BPB_BytsPerSec;
int8_t BPB_SecPerClus;
int16_t BPB_RsvdSecCnt;
int8_t BPB_NumFATs;
int16_t BPB_RootEntCnt;
char BS_VolLab[11];
int32_t	BPB_FATSz32;
int32_t BPB_RootClus;
int32_t DataSec;
int32_t BPB_TotSec32;
int32_t CountofClusters;

/*
 *Function		: LBAToOffset
 *Parameters	: The current sector number that points to a block of data
 *Returns		: The value of the address for that block of data
 *Description	: Finds the starting address of a block of data given the sector number
 *				  corresponding to the data block
 */
int32_t LBAToOffset(int32_t sector)
{
	return ( ( sector - 2 ) * BPB_BytsPerSec ) + ( BPB_BytsPerSec * BPB_RsvdSecCnt ) + 
			( BPB_NumFATs * BPB_FATSz32 * BPB_BytsPerSec );
}

/*
 Name: NextLB
 Purpose: Given a logical block address, look up into the first FAT and 
 return the logical block address of the block in the file. If 
 there is no further blocks then reurn -1
 */
int16_t NextLB( uint32_t sector )
{
	uint32_t FATAddress = ( BPB_BytsPerSec * BPB_RsvdSecCnt ) + ( sector * 4 );
	int16_t val;
	fseek( fp, FATAddress, SEEK_SET );
	fread( &val, 2, 1, fp );
	return val;
}

//Converts a filename ex. "file.txt" to a fat filename ex. "FILE    TXT"
string file_to_fatname(char token[])
{
	if ( (string)token == ".." )
	{
		string temp;
		for (unsigned int i = 0; i < 12; i++)
		{
			if ( i < 2 )
				temp.push_back( '.' );
			else
				temp.push_back( ' ' );
		}
		return temp;
	}
	else if ( (string)token == "." )
	{
		string temp;
		for (unsigned int i = 0; i < 12; i++)
		{
			if ( i < 1 )
				temp.push_back( '.' );
			else
				temp.push_back( ' ' );
		}
		return temp;
	}
	
	string fatFname;
	int i;
	bool file = false;
	
	for ( i = 0; i < 11; i++ )
	{
		if ( token[i] == '.' )
			file = true;
	}
	
	if ( file )
	{
		for ( i = 0; token[i] != '.'; i++ )
		{
			char temp = token[i];
			if ( temp > 90 )
				temp = temp - 32;
				
			fatFname.push_back( temp );
		}
		
		if ( fatFname.size() < 8 )
		{
			unsigned int diff = 8 - fatFname.size();
			for ( unsigned int j = 0; j < diff; j++ )
			{
				fatFname.push_back(' ');
			}
		}
		int lastIndex = i;
		for ( i = lastIndex + 1; i < lastIndex + 4; i++ )
		{
			char temp = token[i];
			if ( temp > 90 )
				temp = temp - 32;
				
			fatFname.push_back( temp );
		}
	}
	else
	{
		for ( i = 0; token[i] != '\0'; i++ ) {}
		int size = i;
		for ( i = 0; i < 11; i++ )
		{
			char temp = token[i];
			if ( temp > 90 )
				temp = temp - 32;
			
			if ( i >= size )
				fatFname.push_back( ' ' );
			else
				fatFname.push_back( temp );
		}
	}
	return fatFname;
}

int main( int argv, char *argc[] )
{
	bool open = false;

	int32_t rootAddress;
	int32_t currentDir;
	dirEntry dirEntries[65536];
	vector<unsigned int> entryCountStack;
	vector<int32_t> dirStack;

	
	char * cmd_str = (char*) malloc( MAX_COMMAND_SIZE );

	while( 1 )
	{
		// Print out the mfs prompt
		cout << "mfs> ";
		
		// Read the command from the commandline.  The
		// maximum command that will be read is MAX_COMMAND_SIZE
		// This while command will wait here until the user
		// inputs something since fgets returns NULL when there
		// is no input
		while( !fgets (cmd_str, MAX_COMMAND_SIZE, stdin) );

		/* Parse input */
		char *token[MAX_NUM_ARGUMENTS];

		int token_count = 0;                                 
	                                                       
		// Pointer to point to the token
		// parsed by strsep
		char *arg_ptr;                                         
	                                                       
		char *working_str  = strdup( cmd_str );                

		// we are going to move the working_str pointer so
		// keep track of its original value so we can deallocate
		// the correct amount at the end
		char *working_root = working_str;

		// Tokenize the input stringswith whitespace used as the delimiter
		while ( ( (arg_ptr = strsep(&working_str, WHITESPACE ) ) != NULL) && (token_count<MAX_NUM_ARGUMENTS))
		{
			token[token_count] = strndup( arg_ptr, MAX_COMMAND_SIZE );
			if( strlen( token[token_count] ) == 0 )
			{
				token[token_count] = NULL;
			}
			token_count++;
		}
	
		if ( (string)token[0] ==  "open")
		{
			if ( open )
			{
				cout << "Error: File system image already open." << endl;
				continue;
			}
		
			fp = fopen( token[1], "r" );
		
			if ( fp == NULL )
			{
				cout << "Error: File system image not found." << endl;
				continue;
			}
			open = true;
		
			fseek( fp, 3, SEEK_SET );
			fread( &BS_OEMName, 1, 8, fp );

			fseek( fp, 11, SEEK_SET );
			fread( &BPB_BytsPerSec, 1, 2, fp );

			fseek( fp, 13, SEEK_SET );
			fread( &BPB_SecPerClus, 1, 1, fp );

			fseek( fp, 14, SEEK_SET );
			fread( &BPB_RsvdSecCnt, 1, 2, fp );

			fseek( fp, 16, SEEK_SET );
			fread( &BPB_NumFATs, 1, 1, fp );

			fseek( fp, 17, SEEK_SET );
			fread( &BPB_RootEntCnt, 1, 2, fp );

			fseek( fp, 43, SEEK_SET );
			fread( &BS_VolLab, 1, 11, fp );

			fseek( fp, 36, SEEK_SET );
			fread( &BPB_FATSz32, 1, 4, fp );

			fseek( fp, 44, SEEK_SET );
			fread( &BPB_RootClus, 1, 4, fp );
		
			fseek( fp, 32, SEEK_SET );
			fread( &BPB_TotSec32, 1, 4, fp );
		
			CountofClusters = DataSec / BPB_SecPerClus;
		
			rootAddress	= 	( BPB_NumFATs * BPB_FATSz32 * BPB_BytsPerSec ) + 
								( BPB_RsvdSecCnt * BPB_BytsPerSec );
			currentDir = rootAddress;
			dirStack.push_back(rootAddress);
			fseek( fp, rootAddress, SEEK_SET );
			unsigned int i;
			for ( i = 0; i < 16; i++ )
			{
				fread( &dirEntries[i], 1, 32, fp );
			}
			entryCountStack.push_back(i);
			
		}
		else if ( (string)token[0] ==  "close" )
		{
			if ( !open )
			{
				cout << "Error: File system not open." << endl;
				continue;
			}
		
			fclose( fp );
			open = false;
		}
		else if ( (string)token[0] ==  "info" )
		{
			if ( !open )
			{
				cout << "Error: File system image must be open first." << endl;
				continue;
			}
		
			cout << "\t\t\t" << "hex\t" << "dec" << endl
				<< "\tBPB_BytsPerSec:\t0x" << hex << (int)BPB_BytsPerSec << "\t" << dec << (int)BPB_BytsPerSec << endl
				<< "\tBPB_SecPerClus:\t0x" << hex << (int)BPB_SecPerClus << "\t" << dec << (int)BPB_SecPerClus << endl
				<< "\tBPB_RsvdSecCnt:\t0x" << hex << (int)BPB_RsvdSecCnt << "\t" << dec << (int)BPB_RsvdSecCnt << endl
				<< "\t   BPB_NumFATs:\t0x" << hex << (int)BPB_NumFATs << "\t" << dec << (int)BPB_NumFATs << endl
				<< "\t   BPB_FATSz32:\t0x" << hex << (int)BPB_FATSz32 << "\t" << dec << (int)BPB_FATSz32 << endl;
		}
		else if ( (string)token[0] ==  "stat" )
		{
			if ( !open )
			{
				cout << "Error: File system image must be open first." << endl;
				continue;
			}
			
			string fatFname = file_to_fatname(token[1]);
			bool found = false;
			
			for (unsigned int i = 0; i < 16; i++)
			{
				char temp[12];
				strcpy(temp, dirEntries[i].DIR_NAME);
				temp[11] = '\0';
				if ( (string)temp == fatFname )
				{
					found = true;
					cout << "Filename:\t\t\"" << fatFname << "\"" << endl;
					
					switch ( dirEntries[i].DIR_Attr )
					{
						case 0x01 :
							cout << "Attribute:\t\t0x01\tread only" << endl;
							break;
						case 0x10 :
							cout << "Attribute:\t\t0x10\tsubdirectory" << endl;
							break;
						case 0x20 :
							cout << "Attribute:\t\t0x20\tarchive flag" << endl;
							break;
						
						default :
							cout << "Attribute:\t\t" << dirEntries[i].DIR_Attr << endl;
					}
					cout << "Starting cluster:\t" << dirEntries[i].DIR_FirstClusterLow << endl << endl;
				}
			}
			if ( !found )
			{
				cout << "Error: File not found." << endl;
			}
		}
		else if ( (string)token[0] ==  "get" )
		{
			if ( !open )
			{
				cout << "Error: File system image must be open first." << endl;
				continue;
			}
			
			string fatFname = file_to_fatname(token[1]);
			bool found = false;
			
			for (unsigned int i = 0; i < entryCountStack.back(); i++)
			{
				char temp[12];
				strcpy(temp, dirEntries[i].DIR_NAME);
				temp[11] = '\0';
				if ( (string)temp == fatFname )
				{
					found = true;
					if ( dirEntries[i].DIR_Attr == 0x10 )
					{
						cout << "Error: File is a directory." << endl;
						break;
					}
					
					FILE *fptr = fopen(token[1], "w");
					char data[BPB_BytsPerSec];
					int32_t currSector = dirEntries[i].DIR_FirstClusterLow;
					
					while ( true )
					{
						fseek( fp, LBAToOffset( currSector ), SEEK_SET );
						fread( &data, 1, BPB_BytsPerSec, fp );
						fprintf( fptr, "%s", data );
						currSector = NextLB( currSector );
						if ( (int)currSector == -1 )
							break;
					}
					
					fclose( fptr );
				}
			}
			if ( !found )
			{
				cout << "Error: File not found." << endl;
			}
		}
		else if ( (string)token[0] ==  "cd" )
		{
			if ( !open )
			{
				cout << "Error: File system image must be open first." << endl;
				continue;
			}
			
			if ( (string)token[1] == ".." )
			{
				dirStack.pop_back();
				entryCountStack.pop_back();
				fseek( fp, dirStack.back(), SEEK_SET );
			
				for ( unsigned int i = 0; i < entryCountStack.back(); i++ )
				{
					fread( &dirEntries[i], 1, 32, fp );
				}
				
				continue;
			}
			else if ( (string)token[1] == "." )
			{
				continue;
			}
			
			string fatFname = file_to_fatname(token[1]);
			bool found = false;
			for (unsigned int i = 0; i < 16; i++)
			{
				char temp[12];
				strcpy(temp, dirEntries[i].DIR_NAME);
				temp[11] = '\0';
				if ( (string)temp == fatFname )
				{
					found = true;
					currentDir = LBAToOffset( dirEntries[i].DIR_FirstClusterLow );
					dirStack.push_back( currentDir );
					
					fseek( fp, currentDir, SEEK_SET );
					
					unsigned int i;
					for ( i = 0; i < 65536; i++ ) 	//65536 is the max number files allowed
					{ 								//per directory
						fread( &dirEntries[i], 1, 32, fp );
						
						if ( dirEntries[i].DIR_NAME[0] == 0x00 )
						{
							break;
						}
					}
					entryCountStack.push_back(i);
				}
			}
			if ( !found )
			{
				cout << "Error: No such directory." << endl;
			}
		}
		else if ( (string)token[0] ==  "ls" )
		{
			if ( !open )
			{
				cout << "Error: File system image must be open first." << endl;
				continue;
			}
			
			if ( token[1] != NULL )
			{
				if ( (string)token[1] == ".." )
				{
					if ( currentDir == rootAddress )
					{
						cout << "Error: Currently in root directory" <<endl;
						continue;
					}
					else
					{
						dirStack.pop_back();
						entryCountStack.pop_back();
						dirEntry tempDir[entryCountStack.back()];
						fseek( fp, dirStack.back(), SEEK_SET );
					
						for ( unsigned int i = 0; i < entryCountStack.back() && i < 100; i++ )
						{
							fread( &tempDir[i], 1, 32, fp );
						}
					
						dirStack.push_back( currentDir );
					
						for ( unsigned int i = 0; i < entryCountStack.back(); i++ )
						{
							if ( tempDir[i].DIR_Attr == 0x01 || tempDir[i].DIR_Attr == 0x10 || tempDir[i].DIR_Attr == 0x20 )
							{
								if ( dirEntries[i].DIR_NAME[0] != 0xE5 || dirEntries[i].DIR_NAME[0] != 0x00 )
								{
									for (unsigned int j = 0; j < 11; j++)
									{
										cout << dirEntries[i].DIR_NAME[j];
									}
									cout << endl;
								}
							}
						}
						cout << endl;
						continue;
					}
						
				}
				else if ( (string)token[1] != "." )
				{
					cout << "Error: Invalid ls parameter." << endl;
					continue;
				}
				else
				{
					for ( unsigned int i = 0; i < entryCountStack.back(); i++ )
					{
						if ( dirEntries[i].DIR_Attr == 0x01 || dirEntries[i].DIR_Attr == 0x10 
							|| dirEntries[i].DIR_Attr == 0x20 )
						{
							if ( dirEntries[i].DIR_NAME[0] != 0xE5 || dirEntries[i].DIR_NAME[0] != 0x00 )
							{
								for (unsigned int j = 0; j < 11; j++)
								{
									cout << dirEntries[i].DIR_NAME[j];
								}
								cout << endl;
							}
						}
					}
					cout << endl;
				}
			}
			else
			{
				for ( unsigned int i = 0; i < entryCountStack.back(); i++ )
				{
					if ( dirEntries[i].DIR_Attr == 0x01 || dirEntries[i].DIR_Attr == 0x10 
						|| dirEntries[i].DIR_Attr == 0x20 )
					{
						for (unsigned int j = 0; j < 11; j++)
						{
							cout << dirEntries[i].DIR_NAME[j];
						}
						cout << endl;
					}
				}
				cout << endl;
			}
		}
		else if ( (string)token[0] ==  "read")
		{
			if ( !open )
			{
				cout << "Error: File system image must be open first." << endl;
				continue;
			}
			if ( token[1] == NULL || token [2] == NULL )
			{
				cout << "Error: Invalid number of arguments" << endl;
			}
			stringstream tempString1( (string)token[2] );
			stringstream tempString2( (string)token[3] );
			int pos, numBytes;
			tempString1 >> pos;
			tempString2 >> numBytes;
			
			string fatFname = file_to_fatname(token[1]);
			bool found = false;
			
			for (unsigned int i = 0; i < entryCountStack.back(); i++)
			{
				char temp[12];
				strcpy(temp, dirEntries[i].DIR_NAME);
				temp[11] = '\0';
				if ( (string)temp == fatFname )
				{
					found = true;
					if ( dirEntries[i].DIR_Attr == 0x10 )
					{
						cout << "Error: File is a directory." << endl;
						break;
					}
					
					char file[ dirEntries[i].DIR_FileSize ];
					char temp[512];
					int32_t currSector = dirEntries[i].DIR_FirstClusterLow;
					
					while ( true )
					{
						fseek( fp, LBAToOffset( currSector ), SEEK_SET );
						fread( &temp, 1, BPB_BytsPerSec, fp );
						strcat(file, temp);
						currSector = NextLB( currSector );
						if ( (int)currSector == -1 )
							break;
					}
					
					for (int j = 0; j < numBytes; j++)
					{
						cout << file[ pos + j ];
					}
					cout << endl << endl;
				}
			}
			if ( !found )
			{
				cout << "Error: File not found." << endl;
			}
			
		}
		else if ( (string)token[0] ==  "volume")
		{
			if ( !open )
			{
				cout << "Error: File system image must be open first." << endl;
				continue;
			}
		
			if ( BS_VolLab[0] == '\0' )
				cout << "Error: Volume name not found." << endl;
			else
				cout << "Volume: " << BS_VolLab << endl;
		}
		else if ( (string)token[0] ==  "exit")
		{
		
			exit( 0 );
		}
		else
		{
			cout << "Invalid command!" << endl;
		}

		free( working_root );

	}
	return 0;
}
