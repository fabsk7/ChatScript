#include "common.h"

/*
:testpattern ( a  _( dog ) ) a big dog
:testpattern ( [ _alternate_1 _alternate_2 ] _* _( dog ) ) alternate_2 cat dog
:testpattern ( _* _(dog) ) cat dog
:testpattern ( _* _one _* ( _two )  _* ( _three ) ) first one two  and three
:testpattern ( _* ( _one _* ( _two ) ) _* ( _three ) ) first one two three
:testpattern ( _* _( _one _two _three ) _* _( _four _* _six ) _* ) one two three middle four five six last
:testpattern ( _* ( _one _* ( _two _* ( _omega ) ) _three _* _four ) ) one two omega three four
:testpattern ( _{ optional } _* _( dog ) ) optional cat dog
:testpattern ( { _optional } _* _( dog ) ) optional cat dog
:testpattern ( _{ _optional } _* _( dog ) ) optional cat dog
:testpattern ( [ _alternate_1 alternate_2 ] _* _( dog ) ) alternate_1 cat dog
:testpattern ( _one _two _( three _four ) five six @_3- _three ) one two three four five six
:testpattern ( _{ _( one _two three ) } _[ _(four five six) ] ) one two three four five six
:testpattern ( _{ _( one _two three ) } _[ _(four five _six) ] ) one two three four five six
*/

#define INFINITE_MATCH (-(200 << 8)) // allowed to match anywhere

#define NOT_BIT			0X00010000
#define FREEMODE_BIT	0X00020000
#define QUOTE_BIT		0X00080000
#define WILDGAP					0X20000000  // start of gap is 0x000000ff, limit of gap is 0x0000ff00  
#define WILDMEMORIZEGAP			0X40000000  // start of gap is 0x000000ff, limit of gap is 0x0000ff00  
#define WILDMEMORIZESPECIFIC	0X80000000  //   while 0x1f0000 is wildcard index to use
#define GAP_SHIFT 16
#define SPECIFIC_SHIFT 24
#define GAPLIMITSHIFT 8
#define NOTNOT_BIT		0X00400000

bool matching = false;
bool clearUnmarks = false;
bool deeptrace = false;
static char* returnPtr = NULL;

static bool MatchTest(bool reverse,WORDP D, int start,char* op, char* compare,int quote,bool &uppercasematch, int&
	actualStart, int& actualEnd, bool exact = false);

// pattern macro  calling data
static unsigned int functionNest = 0;	// recursive depth of macro calling
#define MAX_PAREN_NEST 50
static char* ptrStack[MAX_PAREN_NEST];
static int argStack[MAX_PAREN_NEST];
static int baseStack[MAX_PAREN_NEST];
static int fnVarBaseStack[MAX_PAREN_NEST];
static uint64 matchedBits[20][4];	 // nesting level zone of bit matches

static void MarkMatchLocation(int start, int end, int depth)
{
	for (int i = start; i <= end; ++i)
	{
		int offset = i / 64; // which unit
		int index = i % 64;  // which bit
		uint64 mask = 1 << index;
		matchedBits[depth][offset] |= mask;
	}
}

static char* BitIndex(uint64 bits, char* buffer, int offset)
{
	uint64 mask = 0X0000000000000001ULL;
	for (int index = 0; index <= 63; ++index)
	{
		if (mask & bits) 
		{
			sprintf(buffer,"%d ",offset + index);
			buffer += strlen(buffer);
		}
		mask <<= 1;
	}
	return buffer;
}

void GetPatternData(char* buffer)
{
	char* original = buffer;
	buffer = BitIndex(matchedBits[0][0],buffer,0);
	buffer = BitIndex(matchedBits[0][1],buffer,64);
	buffer = BitIndex(matchedBits[0][2],buffer,128);
	buffer = BitIndex(matchedBits[0][3],buffer,192);
}

static void DecodeFNRef(char* side)
{
	char* at = "";
	if (side[1] == '$') at = GetUserVariable(side+1); 
	else if (IsDigit(side[1])) at = callArgumentList[side[1]-'0'+fnVarBase];
	at = SkipWhitespace(at);
	strcpy(side,at);
}

static void DecodeComparison(char* word, char* lhs, char* op, char* rhs)
{
	// get the operator
	char* compare = word + Decode(word+1,true); // use accelerator to point to op in the middle
	strncpy(lhs,word+2,compare-word-2);
	lhs[compare-word-2] = 0;
	*op = *compare++;
	op[1] = 0;
	if (*compare == '=') // was == or >= or <= or &= 
	{
		op[1] = '=';
		op[2] = 0;
		++compare;
	}
	strcpy(rhs,compare);
}

bool MatchesPattern(char* word, char* pattern) //   does word match pattern of characters and *
{
	if (!*pattern && *word) return false;	// no more pattern but have more word so fails 
	size_t len = 0;
	while (IsDigit(*pattern)) len = (len * 10) + *pattern++ - '0'; //   length test leading characters can be length of word
	if (len && strlen(word) != len) return false; // length failed
	char* start = pattern;

	--pattern;
	while (*++pattern && *pattern != '*' && *word) //   must match leading non-wild exactly
	{
		if (*pattern != '.' &&  *pattern != GetLowercaseData(*word)) return false; // accept a single letter either correctly OR as 1 character wildcard
		++word;
	}
	if (pattern == start && len) return true;	// just a length test, no real pattern
	if (!*word) return !*pattern || (*pattern == '*' && !pattern[1]);	// the word is done. If pattern is done or is just a trailing wild then we are good, otherwise we are bad.
	if (*word && !*pattern) return false;		// pattern ran out w/o wild and word still has more

	// Otherwise we have a * in the pattern now and have to match it against more word
	
	//   wildcard until does match
	char find = *++pattern; //   the characters AFTER wildcard
	if (!find) return true; // pattern ended on wildcard - matches all the rest of the word including NO rest of word

	// now resynch
	--word;
	while (*++word)
	{
		if (*pattern == GetLowercaseData(*word) && MatchesPattern(word + 1,pattern + 1)) return true;
	}
	return false; // failed to resynch
}

static bool SysVarExists(char* ptr) //   %system variable
{
	char* sysvar = SystemVariable(ptr,NULL);
	if (!*sysvar) return false;
	return (*sysvar) ? true : false;	// value != null
}

static bool FindPartialInSentenceTest(char* test, int start,int originalstart,bool reverse,
	int& actualStart, int& actualEnd)
{
	if (!test || !*test) return false;
	if (reverse)
	{
		for ( int i = originalstart-1; i >= 1; --i) // can this be found in sentence backwards
		{
			char word[MAX_WORD_SIZE];
			MakeLowerCopy(word,wordStarts[i]);
			if (unmarked[i] || !MatchesPattern(word,test)) continue;	// if universally unmarked, skip it. Or if they dont match
			// we have a match of a word
			actualStart = i;
			actualEnd = i;
			return true;
		}
	}
	else 
	{
		for (int i = start+1; i <= wordCount; ++i) // can this be found in sentence
		{
			char word[MAX_WORD_SIZE];
			MakeLowerCopy(word,wordStarts[i]);
			if (unmarked[i] || !MatchesPattern(word,test)) continue;	// if universally unmarked, skip it. Or if they dont match
			// we have a match of a word
			actualStart = i;
			actualEnd = i;
			return true;
		}
	}
	return false;
}

static bool MatchTest(bool reverse,WORDP D, int start,char* op, char* compare,int quote,bool &uppercasematch,
	int& actualStart, int& actualEnd, bool exact) // is token found somewhere after start?
{
	uppercasematch = false;
	if (deeptrace) Log(STDUSERLOG,(char*)" matchtesting:%s ",D->word);
	while (GetNextSpot(D,start,actualStart,actualEnd,reverse)) // find a spot later where token is in sentence
    {
		if (deeptrace) Log(STDUSERLOG,(char*)" matchtest:%s %d-%d ",D->word,actualStart,actualEnd);
		if (exact && (start+1) != actualStart) return false;	// we are doing _0?~hello or whatever. Must be on the mark
 		if (deeptrace) Log(STDUSERLOG,(char*)" matchtest:ok ");
        start = actualStart; // where to try next if fail on test
        if (op) // we have a test to perform
        {
			char* word;
			if (D->word && (IsAlphaUTF8(*D->word) || D->internalBits & UTF8)) word = D->word; //   implicitly all normal words are relation tested as given
			else word = quote ? wordStarts[actualStart] : wordCanonical[actualStart];
			int id;
			if (deeptrace) Log(STDUSERLOG,(char*)" matchtest:optest ");
			char word1val[MAX_WORD_SIZE];
			char word2val[MAX_WORD_SIZE];
 			if (HandleRelation(word,op,compare,false,id,word1val,word2val) & ENDCODES) continue; // failed 
        }
 		if (*D->word == '~') 
		{
			if (D->internalBits & UPPERCASE_MATCH) uppercasematch = true; //how can this be upper case? BW BUG
			return true; // we CANNOT tell whether original or canon led to set...
		}
       if (!quote) return true; // can match canonical or original
		
        //   we have a match, but prove it is a original match, not a canonical one
		if (actualEnd < actualStart) continue;	// trying to match within a composite. 
		if (actualStart == actualEnd && !stricmp(D->word,wordStarts[actualStart])) return true;   // literal word match
		else // match a phrase literally
		{
			char word[MAX_WORD_SIZE];
			char* at = word;
			for (int i = actualStart; i <= actualEnd; ++i)
			{
				strcpy(at,wordStarts[i]);
				at += strlen(wordStarts[i]);
				if (i != actualEnd) *at++ = '_';
			}
			*at = 0;
			if (!stricmp(D->word,word)) return true;
		}
    } 
	 if (deeptrace) Log(STDUSERLOG,(char*)" matchtest:%s failed ",D->word);
     return false;
}

static bool FindPhrase(char* word, int start,bool reverse, int & actualStart, int& actualEnd)
{   // Phrases are dynamic, might not be marked, so have to check each word separately. -- faulty in not respecting ignored(unmarked) words
	if (start > wordCount) return false;
	bool matched = false;
	actualEnd = start;
	int oldend;
	oldend = start = 0; // allowed to match anywhere or only next

	int n = BurstWord(word);
	for (int i = 0; i < n; ++i) // use the set of burst words - but "Andy Warhol" might be a SINGLE word.
	{
		WORDP D = FindWord(GetBurstWord(i));
		bool junk;
		matched = MatchTest(reverse,D,actualEnd,NULL,NULL,0,junk,actualStart,actualEnd);
		if (matched)
		{
			if (oldend > 0 && actualStart != (oldend + 1)) // do our words match in sequence NO. retry later in sentence
			{
				++start;
				actualStart = actualEnd = start;
				i = -1;
				oldend = start = 0;
				matched = false;
				continue;
			}
			if (i == 0) start = actualStart; // where we matched INITIALLY
			oldend = actualEnd;
		}
		else break;
	}
	if (matched) actualStart = start;
	return matched;
}

bool Match(char* ptr, unsigned int depth, int startposition, char* kind, int wildstart,unsigned int& wildcardSelector,
	int &returnstart,int& returnend,bool &uppercasem,int& firstMatched,int& positionStart,int& positionEnd, bool reverse)
{//   always STARTS past initial opening thing ( [ {  and ends with closing matching thing
	int startdepth = globalDepth;
	memset(&matchedBits[depth],0,sizeof(uint64) * 4);  // nesting level zone of bit matches
    char word[MAX_WORD_SIZE];
	char* orig = ptr;
	int statusBits = (*kind == '<') ? FREEMODE_BIT : 0; //   turns off: not, quote, startedgap, freemode ,wildselectorpassback
    if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERTABLOG, "%s ",kind); //   start on new indented line
	ChangeDepth(1,(char*)"Match");
    bool matched;
	unsigned int startNest = functionNest;
	unsigned int result;
    WORDP D;
	unsigned int oldtrace = trace;
	bool oldecho = echo;
	bool success = false;
    firstMatched = -1; //   ()  should return spot it started (firstMatched) so caller has ability to bind any wild card before it
    if (wildstart == 1)  positionStart = INFINITE_MATCH; //   INFINITE_MATCH means we are in initial startup, allows us to match ANYWHERE forward to start
    positionEnd = startposition; //   we scan starting 1 after this
 	int basicStart = startposition;	//   we must not match real stuff any earlier than here
    char* argumentText = NULL; //   pushed original text from a function arg -- function arg never decodes to name another function arg, we would have expanded it instead
    bool uppercasematch = false;
	while (ALWAYS) //   we have a list of things, either () or { } or [ ].  We check each item until one fails in () or one succeeds in  [ ] or { }
    {
        int oldStart = positionStart; //  allows us to restore if we fail, and confirm legality of position advance.
        int oldEnd = positionEnd;
		int id;
		char* nextTokenStart = SkipWhitespace(ptr);
		returnPtr = nextTokenStart; // where we last were before token... we cant fail on _ and that's what we care about
		ptr = ReadCompiledWord(nextTokenStart,word);
		if (*word == '<' && word[1] == '<')  ++nextTokenStart; // skip the 1st < of <<  form
		if (*word == '>' && word[1] == '>')  ++nextTokenStart; // skip the 1st > of >>  form
		nextTokenStart = SkipWhitespace(nextTokenStart+1);	// ignore blanks after if token is a simple single thing like !

		char c = *word;
		if (deeptrace) Log(STDUSERLOG,(char*)" token:%s ",word);
        switch(c) 
        {
			// prefixs on tokens
            case '!': //   NOT condition - not a stand-alone token, attached to another token
				ptr = nextTokenStart;
				statusBits |= NOT_BIT;
				if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"!");
				if (*ptr == '!') 
				{
					ptr = SkipWhitespace(nextTokenStart+1);
					statusBits |= NOTNOT_BIT;
					if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"!");
				}
				continue;
			case '\'': //   single quoted item    
				if (!stricmp(word,(char*)"'s"))
				{
					matched = MatchTest(reverse,FindWord(word),(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd,NULL,NULL,
						statusBits & QUOTE_BIT,uppercasematch,positionStart,positionEnd);
					if (!matched || !(wildcardSelector & WILDMEMORIZESPECIFIC)) uppercasematch = false;
					if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart;
					break;
				}
				else
				{
					statusBits |= QUOTE_BIT;
					ptr = nextTokenStart;
					if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"'");
					continue;
				}
			case '_':
				// a wildcard id?  names a memorized value like _8
				if (IsDigit(word[1]))
				{
					matched = GetwildcardText(GetWildcardID(word),false)[0] != 0; // simple _2  means is it defined
					break;
				}
				// memorization coming - there can be up-to-two memorizations in progress: _* (wildmemorizegap) and _xxx (wildmemorizespecific)
				// it will be gap first and specific second (either token or smear of () [] {} )
				ptr = nextTokenStart;
			
				// if we are going to memorize something AND we previously matched inside a phrase, we need to move to after phrase...
				if ((positionStart - positionEnd) == 1 && !reverse) positionEnd = positionStart; // If currently matched a phrase, move to end. 
				else if ((positionEnd - positionStart) == 1 && reverse) positionStart = positionEnd; // If currently matched a phrase, move to end. 
				
				uppercasematch = false;

				//  aba or ~dat
				if (ptr[0] != '*' ) 
				{
					wildcardSelector |= (WILDMEMORIZESPECIFIC + (wildcardIndex << SPECIFIC_SHIFT)); 
				}
				// *1 or *-2 or *elle (why allow that?)
				else if (IsDigit(ptr[1]) ||  ptr[1] == '-' || IsAlphaUTF8(ptr[1])) 
				{
					wildcardSelector |= (WILDMEMORIZESPECIFIC + (wildcardIndex << SPECIFIC_SHIFT)); 
				}
				else // *~ or *
				{
					wildcardSelector |=  (WILDMEMORIZEGAP + (wildcardIndex << GAP_SHIFT)); // variable gap
				}
				SetWildCardNull(); // dummy match to reserve place
				if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"_");
				continue;
			case '@': // factset ref
				if (word[1] == '_') // set positional reference  @_20+ or @_0-   
				{
					if (firstMatched < 0) firstMatched = NORETRY; // cannot retry this match locally
	
					// memorize gap to end based on direction...
					if ((wildcardSelector & WILDMEMORIZEGAP) && !reverse) // close to end of sentence 
					{
						positionStart = wordCount; // pretend to match at end of sentence
						int start = wildcardSelector & 0x000000ff;
						int limit = (wildcardSelector >> GAPLIMITSHIFT) & 0x000000ff;
  						if ((positionStart + 1 - start) > limit) //   too long til end
						{
							matched = false;
 							wildcardSelector &= -1 ^ (WILDMEMORIZEGAP | WILDGAP);
							break;
						}
						if (wildcardSelector & WILDMEMORIZEGAP) 
						{
							SetWildCard(start,wordCount,true);  //   legal swallow of gap //   request memorize
 							wildcardSelector &= -1 ^ (WILDMEMORIZEGAP | WILDGAP);
						}
					}

					char* end = word+3;  // skip @_2
					if (IsDigit(*end)) ++end; // point to proper + or - ending
					unsigned int wild = wildcardPosition[GetWildcardID(word+1)];
					if (*end == '+') 
					{
						positionStart = WILDCARD_START(wild);
						positionEnd = WILDCARD_END(wild);
						reverse = false;
					}
					else if (*end == '-') 
					{
						reverse = true;
						positionEnd = positionStart = WILDCARD_START(wild);
					}
					if (!positionEnd) break;
					oldEnd = positionEnd; // forced match ok
					oldStart = positionStart;
					if (trace & TRACE_PATTERN  && CheckTopicTrace()) 
					{
						if (positionStart <= 0 || positionStart > wordCount || positionEnd <= 0 || positionEnd > wordCount) Log(STDUSERLOG, "(index:%d)",positionEnd);
						else if (positionStart == positionEnd) Log(STDUSERLOG,(char*)"(word:%s index:%d)",wordStarts[positionEnd],positionEnd);
						else Log(STDUSERLOG,(char*)"(word:%s-%s index:%d-%d)",wordStarts[positionStart],wordStarts[positionEnd],positionStart,positionEnd);
					}
					matched = true;
				}
				else
				{
					int set = GetSetID(word);
					if (set == ILLEGAL_FACTSET) matched = false;
					else matched = FACTSET_COUNT(set) != 0;
				}
				break;
   			case '<': //   sentence start marker OR << >> set
				if (firstMatched < 0) firstMatched = NORETRY; // cannot retry this match
				if (word[1] == '<') 
					goto DOUBLELEFT; //   << 
                else 
				{
					ptr = nextTokenStart;
					if ((wildcardSelector & WILDGAP) && !reverse) // cannot memorize going forward to  start of sentence
					{
						matched = false;
 						wildcardSelector &= -1 ^ (WILDMEMORIZEGAP|WILDGAP);
					}
					else { // match can FORCE it to go to start from any direction
						positionStart = positionEnd = 0; //   idiom < * and < _* handled under *
						matched = true;
					}
				}
                break;
            case '>': //   sentence end marker
				if (word[1] == '>') 
					goto DOUBLERIGHT; //   >> closer, and reset to start of sentence wild again...
				
				ptr = nextTokenStart;
				if ((wildcardSelector & WILDGAP) && reverse) // cannot memorize going backward to  end of sentence
				{
					matched = false;
 					wildcardSelector &= -1 ^ (WILDMEMORIZEGAP | WILDGAP);
				}
				else if ((wildcardSelector & WILDGAP) || positionEnd == wordCount)// you can go to end from anywhere if you have a gap OR you are there
				{
					matched =  true;
					positionStart = positionEnd = wordCount + 1; //   pretend to match a word off end of sentence
				}
				else if (*kind == '[' || *kind == '{') // nested unit will figure out if legal 
				{
					matched =  true;
					positionStart = positionEnd = wordCount + 1; //   pretend to match a word off end of sentence
				}
				else matched = false;
                break;
             case '*':
				if (word[1] == '-') //   backward grab, -1 is word before now -- BUG does not respect unmark system
				{
					int at = positionEnd - (word[2] - '0') - 1; // limited to 9 back
					if (at >= 0) //   no earlier than pre sentence start
					{
						oldEnd = at; //   set last match BEFORE our word
						positionStart = positionEnd = at + 1; //   cover the word now
						matched = true; 
					}
					else matched = false;
				}
				else if (IsDigit(word[1]))  // fixed length gap
                {
					int at;
					int count = word[1] - '0';	// how many to swallow
					if (reverse)
					{
						int begin = positionStart -1;
						at = positionStart; // start here
						while (count-- && --at >= 1) // can we swallow this (not an ignored word)
						{
							if (unmarked[at]) 
							{
								++count;	// ignore this word
								if (at == begin) --begin;	// ignore this as starter
							}
						}
						if (at >= 1 ) // pretend match
						{ 
							positionEnd = begin ; // pretend match here -  wildcard covers the gap
							positionStart = at; 
							matched = true; 
						}
						else  matched = false;
					}
					else
					{
						at = positionEnd; // start here
						int begin = positionEnd + 1;
						while (count-- && ++at <= wordCount) // can we swallow this (not an ignored word)
						{
							if (unmarked[at]) 
							{
								++count;	// ignore this word
								if (at == begin) ++begin;	// ignore this as starter
							}
						}
						if (at <= wordCount ) // pretend match
						{ 
							positionStart = begin; // pretend match here -  wildcard covers the gap
 							positionEnd = at; 
							matched = true; 
						}
						else  matched = false;
					}
                }
				else if (IsAlphaUTF8(word[1]) || word[1] == '*') 
					matched = FindPartialInSentenceTest(word+1,(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd,positionStart,reverse,
					positionStart,positionEnd); // wildword match like st*m* or *team* matches steamroller
                else // variable gap
                {
					int start = (reverse) ? (positionStart  - 1) : (positionEnd  + 1);
					wildcardSelector |= start | WILDGAP; // cannot conflict, two wilds in a row change no position
					if (word[1] == '~') 
					{
						wildcardSelector |= (word[2]-'0') << GAPLIMITSHIFT; // *~3 - limit is 9 back
					}
                    else // I * meat
					{
						wildcardSelector |= 200 << GAPLIMITSHIFT;  // 200 is a safe infinity // I * meat
						if (positionStart == 0) positionStart = INFINITE_MATCH; // < * resets to allow match anywhere
					}
					if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"%s ",word);
					continue;
                }
                break;
            case '$': // is user variable defined
				{
					char* val = GetUserVariable(word);
					matched = *val ? true : false;
				}
                break;
            case '^': //   function call, function argument  or indirect function variable assign ref like ^$$tmp = null
                 if  (IsDigit(word[1]) || word[1] == '$' || word[1] == '_') //   macro argument substitution or indirect function variable
                {
                    argumentText = ptr; //   transient substitution of text

					if (IsDigit(word[1]))  ptr = callArgumentList[word[1]-'0'+fnVarBase];  // nine argument limit
					else if (word[1] == '$') ptr = GetUserVariable(word+1); // get value of variable and continue in place
					else ptr = wildcardCanonicalText[GetWildcardID(word+1)]; // ordinary wildcard substituted in place (bug)?
					if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"%s=>",word);
					continue;
                }
                
				D = FindWord(word,0); // find the function
				if (!D || !(D->internalBits & FUNCTION_NAME)) matched = false; // shouldnt fail
				else if (D->x.codeIndex) // system function - execute it
                {
					char* old = currentOutputBase;
					char* oldrule = currentRuleOutputBase;
					currentRuleOutputBase = currentOutputBase = AllocateBuffer(); // start an independent buffer
					FunctionResult result;
					matching = true;
					ptr = DoFunction(word,ptr,currentOutputBase,result);
					matching = false;
					matched = !(result & ENDCODES); 

					// allowed to do comparisons on answers from system functions but cannot have space before them, but not from user macros
					if (*ptr == '!' && ptr[1] == ' ' ){;} // simple not operator
					else if (ptr[1] == '<' || ptr[1] == '>'){;} // << and >> are not comparison operators in a pattern
					else if (IsComparison(*ptr) && *(ptr-1) != ' ' && (*ptr != '!' || ptr[1] == '='))  // ! w/o = is not a comparison
					{
						char op[10];
						char* opptr = ptr;
						*op = *opptr;
						op[1] = 0;
						char* rhs = ++opptr; 
						if (*opptr == '=') // was == or >= or <= or &= 
						{
							op[1] = '=';
							op[2] = 0;
							++rhs;
						}
						char copy[MAX_WORD_SIZE];
						ptr = ReadCompiledWord(rhs,copy);
						rhs = copy;

						if (*rhs == '^') // local function argument or indirect ^$ var  is LHS. copy across real argument
						{
							char* at = "";
							if (rhs[1] == '$') at = GetUserVariable(rhs+1); 
							else if (IsDigit(rhs[1])) at = callArgumentList[rhs[1]-'0'+fnVarBase];
							at = SkipWhitespace(at);
							strcpy(rhs,at);
						}
				
						if (*op == '?' && opptr[0] != '~')
						{
							bool junk;
							matched = MatchTest(reverse,FindWord(currentOutputBase),
								(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd,NULL,NULL,false,junk,
								positionStart,positionEnd); 
							if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart; //   first SOLID match
						}
						else
						{
							int id;
							char word1val[MAX_WORD_SIZE];
							char word2val[MAX_WORD_SIZE];
 							result = HandleRelation(currentOutputBase,op,rhs,false,id,word1val,word2val); 
							matched = (result & ENDCODES) ? 0 : 1;
						}
					}
					FreeBuffer();
					currentOutputBase = old;
					currentRuleOutputBase = oldrule;
                }
				else // user function - execute it in pattern context as continuation of current code
				{ 
					if (functionNest >= MAX_PAREN_NEST) // fail, too deep nesting
					{
						matched = false;
						break; 
					}

					//   save old base data
					baseStack[functionNest] = callArgumentBase; 
					argStack[functionNest] = callArgumentIndex; 
					fnVarBaseStack[functionNest] = fnVarBase;

					if ((trace & TRACE_PATTERN || D->internalBits & MACRO_TRACE)  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"((char*)"); 
					ptr += 2; // skip ( and space
					// read arguments
					while (*ptr && *ptr != ')' ) 
					{
						char* arg = callArgumentList[callArgumentIndex++];
						ptr = ReadArgument(ptr,arg);  // gets the unevealed arg
						if ((trace & TRACE_PATTERN || D->internalBits & MACRO_TRACE)  && CheckTopicTrace()) Log(STDUSERLOG,(char*)" %s, ",arg); 
					}
					if ((trace & TRACE_PATTERN || D->internalBits & MACRO_TRACE)  && CheckTopicTrace()) Log(STDUSERLOG,(char*)")\r\n"); 
					fnVarBase = callArgumentBase = argStack[functionNest];
					ptrStack[functionNest++] = ptr+2; // skip closing paren and space
					ptr = (char*) D->w.fndefinition + 1; // continue processing within the macro, skip argument count
					oldecho = echo;
					oldtrace = trace;
					if (D->internalBits & MACRO_TRACE  && CheckTopicTrace()) 
					{
						trace = (unsigned int)-1;
						echo = true;
					}
					if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERLOG,(char*)"%s=> ",word);
					continue;
				}
				break;
          case 0: // end of data (argument or function - never a real rule)
	           if (argumentText) // return to normal from argument substitution
                {
                    ptr = argumentText;
                    argumentText = NULL;
                    continue;
                }
                else if (functionNest > startNest) // function call end
                {
 					if (trace & TRACE_PATTERN  && CheckTopicTrace()) Log(STDUSERTABLOG,(char*)""); 
					--functionNest;
                    callArgumentIndex = argStack[functionNest]; //   end of argument list (for next argument set)
                    callArgumentBase = baseStack[functionNest]; //   base of callArgumentList
                    fnVarBase = fnVarBaseStack[functionNest];
					ptr = ptrStack[functionNest]; // continue using prior code
					trace = oldtrace;
                    echo = oldecho;
					continue;
                }
                else 
				{
					ChangeDepth(-1,(char*)"Match");
					globalDepth = startdepth;
 					return false; // shouldn't happen
				}
                break;
DOUBLELEFT:  case '(': case '[':  case '{': // nested condition (required or optional) (= consecutive  [ = choice   { = optional   << all of
				// we make << also a depth token
				ptr = nextTokenStart;
				{
					int returnStart = positionStart;
					int returnEnd = positionEnd;
					int rStart = positionStart;
					int rEnd = positionEnd;
					unsigned int oldselect = wildcardSelector;
					wildcardSelector = 0; // inside starts fresh on memorization and gaps
					bool uppercasemat = false;
					// nest inherits gaps leading to it. memorization requests withheld til he returns
					int whenmatched = 0;
					char* type = "[";
					if (*word == '(') type = "(";
					else if (*word == '{') type = "{";
					else if (*word == '<') 
					{
						type = "<<";
						positionEnd = startposition;  //   allowed to pick up after here - oldStart/oldEnd synch automatically works
						positionStart = INFINITE_MATCH;
						rEnd = 0;
						rStart = INFINITE_MATCH; 
					}
					int wildstarter = 0;
					if (positionStart == INFINITE_MATCH) wildstarter = 1;
					if (oldselect & WILDGAP) wildstarter = 2; // allowed to gap in
					matched = Match(ptr,depth+1,positionEnd,type, wildstarter,wildcardSelector,returnStart,
						returnEnd,uppercasemat,whenmatched,positionStart,positionEnd,reverse); //   subsection ok - it is allowed to set position vars, if ! get used, they dont matter because we fail
					wildcardSelector = oldselect; // restore outer environment
					if (matched) 
					{
						// copy back marking bits on match
						if (!(statusBits & NOT_BIT)) // wanted match to happen
						{
							int olddepth = depth + 1;
							matchedBits[depth][0] |= matchedBits[olddepth][0];
							matchedBits[depth][1] |= matchedBits[olddepth][1];
							matchedBits[depth][2] |= matchedBits[olddepth][2];
							matchedBits[depth][3] |= matchedBits[olddepth][3];
						}
						if (!(statusBits & NOT_BIT)  && firstMatched < 0) firstMatched = whenmatched;
						positionStart = returnStart;
						if (positionStart == INFINITE_MATCH && returnStart > 0 &&  returnStart != INFINITE_MATCH) positionStart = returnEnd;
						positionEnd = returnEnd;
						if (*word == '<') // allows thereafter to be anywhere
						{
							positionStart = INFINITE_MATCH;
							oldEnd = oldStart = positionEnd = 0;
						}
						uppercasematch = uppercasemat;
						// The whole thing matched but if @_ was used, which way it ran and what to consider the resulting zone is completely confused.
						// So force a tolerable meaning so it acts like it is contiguous to caller.  If we are memorizing it may be silly but its what we can do.
						if (*word == '(' && positionStart == NORETRY) 
						{
							positionEnd = positionStart = (reverse) ? (oldStart - 1) : (oldEnd + 1) ;  // claim we only moved 1 unit
						}
						else if (positionEnd) oldEnd = (reverse) ? (positionEnd + 1) : (positionEnd - 1); //   nested checked continuity, so we allow match whatever it found - but not if never set it (match didnt have words)
					}
					else if (*word == '{')  // we didnt match, but we are not required to
					{
						if (wildcardSelector & WILDMEMORIZESPECIFIC) // was already inited to null when slot allocated
						{ // what happens with ( *~3 {boy} bottle) ? // not legal
							wildcardSelector ^= WILDMEMORIZESPECIFIC; // do not memorize it further
						}
                        statusBits |= NOT_BIT; //   we didnt match and pretend we didnt want to
					}
					else // no match for ( or [ or << means we have to restore old positions regardless of what happened inside
					{ // but we should check why the positions were not properly restored from the match call...BUG
						positionStart = rStart;
						positionEnd = rEnd;
						wildcardSelector = 0; // failure of [ and ( and << loses all memorization
					}
				} // just a data block
				ptr = BalanceParen(ptr,true,false); // reserve wildcards as we skip over the material including closer 
                break;

 DOUBLERIGHT: case ')': case ']': case '}' :  //   end sequence/choice/optional
				ptr = nextTokenStart;
				matched = (*kind == '(' || *kind == '<'); //   [] and {} must be failures if we are here while ( ) and << >> are success
				if (wildcardSelector & WILDGAP) //   pending gap  -  [ foo fum * ] and { foo fot * } are  illegal but [*3 *2] is not 
                {
					if (depth != 0) //  don't end with a gap, illegal
					{
						wildcardSelector = 0;
						matched = false; //   force match failure
					}
					else positionStart = wordCount + 1; //   at top level a close implies > )
				}
                break; 
            case '"':  //   double quoted string
				matched = FindPhrase(word,(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd, reverse,
					positionStart,positionEnd);
				if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart; //   first SOLID match
				break;
            case '%': //   system variable
				if (!word[1]) // simple % 
				{
					bool junk;
					matched = MatchTest(reverse,FindWord(word),(positionEnd < basicStart && firstMatched < 0) ? basicStart: positionEnd,NULL,NULL,
						statusBits & QUOTE_BIT,junk,positionStart,positionEnd); //   possessive 's
					if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart; //   first SOLID match
				}
                else matched = SysVarExists(word);
                break;
            case '?': //  question sentence? 
				ptr = nextTokenStart;
				if (!word[1]) matched = (tokenFlags & QUESTIONMARK) ? true : false;
				else matched = false;
	            break;
            case '=': //   a comparison test - never quotes the left side. Right side could be quoted
				//   format is:  = 1-bytejumpcodeToComparator leftside comparator rightside
				if (!word[1]) //   the simple = being present
				{
					bool junk;
					matched = MatchTest(reverse,FindWord(word),(positionEnd < basicStart && firstMatched < 0)  ? basicStart : positionEnd,NULL,NULL,
						statusBits & QUOTE_BIT,junk,positionStart,positionEnd); //   possessive 's
					if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart; //   first SOLID match
				}
				//   if left side is anything but a variable $ or _ or @, it must be found in sentence and that is what we compare against
				else 
				{
					char lhsside[MAX_WORD_SIZE];
					char* lhs = lhsside;
					char op[10];
					char rhsside[MAX_WORD_SIZE];
					char* rhs = rhsside;
					DecodeComparison(word, lhs, op, rhs);
					if (trace) sprintf(word,(char*)"%s%s%s",lhs,op,rhs);
					if (*lhs == '^') DecodeFNRef(lhs); // local function arg indirect ^$ var or _ as LHS
					if (*rhs == '^') DecodeFNRef(rhs);// local function argument or indirect ^$ var  is LHS. copy across real argument
				
					bool quoted = false;
					if (*lhs == '\'') // left side is quoted
					{
						++lhs; 
						quoted = true;
					}
			
					if (*op == '?' && *rhs != '~') // NOT a ? into a set test - means does this thing exist in sentence
					{
						char* val = "";
						if (*lhs == '$') val = GetUserVariable(lhs);
						else if (*lhs == '_') val = (quoted) ? wildcardOriginalText[GetWildcardID(lhs)] : wildcardCanonicalText[GetWildcardID(lhs)];
						else if (*lhs == '^' && IsDigit(lhs[1])) val = callArgumentList[lhs[1]-'0'+fnVarBase];  // nine argument limit
						else if (*lhs == '%') val = SystemVariable(lhs,NULL);
						else val = lhs; // direct word

						if (*val == '"') // phrase requires dynamic matching
						{
							matched = FindPhrase(val,(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd, reverse,
								positionStart,positionEnd);
							if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart; //   first SOLID match
							if (trace) sprintf(word,(char*)"%s(%s)%s",lhs,val,op);
							break;
						}

						bool junk;
						matched = MatchTest(reverse,FindWord(val),(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd,NULL,NULL,
							quoted,junk,positionStart,positionEnd); 
						if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart; //   first SOLID match
						if (trace) sprintf(word,(char*)"%s(%s)%s",lhs,val,op);
						break;
					}
	
					result = *lhs;
					if (result == '%' || result == '$' || result == '_' || result == '@' || (*op == '?' && rhs)) // otherwise for words and concepts, look up in sentence and check relation there
					{
						if (result == '_' && quoted) --lhs; // include the quote
						char word1val[MAX_WORD_SIZE];
						char word2val[MAX_WORD_SIZE];
						FunctionResult answer = HandleRelation(lhs,op,rhs,false,id,word1val,word2val); 
						matched = (answer & ENDCODES) ? 0 : 1;
						if (trace) 
						{
							if (!stricmp(lhs,word1val)) *word1val = 0; // dont need redundant constants in trace
							if (!stricmp(rhs,word2val)) *word2val = 0; // dont need redundant constants in trace
							if (*word1val && *word2val) sprintf(word,(char*)"%s(%s)%s%s(%s)",lhs,word1val,op,rhs,word2val);
							else if (*word1val) sprintf(word,(char*)"%s(%s)%s%s",lhs,word1val,op,rhs);
							else if (*word2val) sprintf(word,(char*)"%s%s%s(%s)",lhs,op,rhs,word2val);
							else sprintf(word,(char*)"%s%s%s",lhs,op,rhs);
						}
					}
					else // find and test
					{
						bool junk;
						matched = MatchTest(reverse,FindWord(lhs),(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd,op,rhs,
							quoted,junk,positionStart,positionEnd); //   MUST match later 
						if (!matched) break;
					}
 				}
				break;
            case '\\': //   escape to allow [ ] () < > ' {  } ! as words and 's possessive And anything else for that matter
				{
					bool junk;
					matched =  MatchTest(reverse,FindWord(word+1),(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd,NULL,NULL,
						statusBits & QUOTE_BIT,junk,positionStart,positionEnd);
					if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart; 
					if (matched) {}
					else if (word[1] == '!' ) matched =  (wordCount && (tokenFlags & EXCLAMATIONMARK)); //   exclamatory sentence
  					else if (word[1] == '?') matched =  (tokenFlags & QUESTIONMARK) ? true : false; //   question sentence
					break;
				}
			case '~': // current topic ~ and named topic
				if (word[1] == 0) // current topic
				{
					matched = IsCurrentTopic(currentTopicID); // clearly we are executing rules from it but is the current topic interesting
					break;
				}
				// drop thru for all other ~
			default: //   ordinary words, concept/topic, numbers, : and ~ and | and & accelerator
				matched = MatchTest(reverse,FindWord(word),(positionEnd < basicStart && firstMatched < 0) ? basicStart : positionEnd,NULL,NULL,
					statusBits & QUOTE_BIT,uppercasematch,positionStart,positionEnd);
				if (!matched || !(wildcardSelector & WILDMEMORIZESPECIFIC)) uppercasematch = false;
				if (!(statusBits & NOT_BIT) && matched && firstMatched < 0) firstMatched = positionStart;
				if (matched && !(statusBits & NOT_BIT)) MarkMatchLocation(positionStart, positionEnd,depth);
         } 
		statusBits &= -1 ^ QUOTE_BIT; // turn off any pending quote
        if (statusBits & NOT_BIT) // flip success to failure maybe
        {
			if (matched)
			{
				if (statusBits & NOTNOT_BIT) // is match immediately after or not
				{
					if (!reverse && positionStart == (oldEnd + 1)) matched = uppercasematch = false;
					else if (reverse && positionEnd == (oldStart - 1)) matched = uppercasematch = false;
				}
				else uppercasematch = matched = false; 
				statusBits &= -1 ^ (NOT_BIT|NOTNOT_BIT);
				positionStart = oldStart; //   restore any changed position values (if we succeed we would and if we fail it doesnt harm us)
				positionEnd = oldEnd;
			}
        }

		//   prove any wild gap was legal, accounting for ignored words if needed
 		unsigned int started;
		if (!reverse) started = (positionStart < REAL_SENTENCE_LIMIT) ? positionStart : 0; // position start may be the unlimited access value
		else started = (positionStart < REAL_SENTENCE_LIMIT) ? positionEnd : wordCount; // position start may be the unlimited access value
		if (started == INFINITE_MATCH) started = 1;
		bool legalgap = false;
		unsigned int memorizationStart = positionStart;
        if ((wildcardSelector & WILDGAP) && matched) // test for legality of gap
        {
			unsigned int begin = started; // where we think we are now
			memorizationStart = started = (wildcardSelector & 0x000000ff); // actual word we started at
			unsigned int ignore = started;
			int x;
			int limit = (wildcardSelector >> 8) & 0x000000ff; 
			if (reverse)
			{
				x = started - begin; // *~2 debug() something will generate a -1 started... this is safe here
				while (ignore > begin) // no charge for ignored words in gap
				{
					if (unmarked[ignore--]) --x; 
				}
			}
			else
			{
				x = begin - started; // *~2 debug() something will generate a -1 started... this is safe here
				while (ignore < begin) // no charge for ignored words in gap
				{
					if (unmarked[ignore++]) --x; 
				}
			}
  			if (x <= limit) legalgap = true;   //   we know this was legal, so allow advancement test not to fail- matched gap is started...oldEnd-1
			else  
			{
				matched = false;  // more words than limit
				wildcardSelector &= -1 ^ (WILDMEMORIZEGAP | WILDGAP); //   turn off any save flag
			}
		}
		if (matched) // perform any memorization
		{
			if (oldEnd == positionEnd && oldStart == positionStart) // something like function call or variable existence, didnt change position
			{
				if (wildcardSelector == WILDMEMORIZESPECIFIC)
				{
					if (*word == '$')
					{
						char* value = GetUserVariable(word);
						SetWildCard(value,value, 0,0);  // specific swallow
					}
				}
			}
			else if (wildcardSelector & (WILDMEMORIZEGAP |WILDMEMORIZESPECIFIC )) //   memorize ONE -  will be gap or specific not in front of () {} []
			{
				if (started == INFINITE_MATCH) started = 1;
				if (positionStart == INFINITE_MATCH) positionStart = 1;
				if (wildcardSelector & WILDMEMORIZEGAP) //   would be first if both
				{
					int index = (wildcardSelector >> GAP_SHIFT) & 0x0000001f;
					if (reverse)
					{
						if ((started - positionEnd) == 0) SetWildCardGivenValue((char*)"",(char*)"",0,positionEnd+1,index); // empty gap
						else SetWildCardGiven(positionEnd + 1,oldStart-1,true,index );  //   wildcard legal swallow between elements
					}	
					else if ((positionStart - memorizationStart) == 0) SetWildCardGivenValue((char*)"",(char*)"",0,oldEnd+1, index); // empty gap
					else SetWildCardGiven(memorizationStart,positionStart-1,true, index);  //   wildcard legal swallow between elements
				}
				if (wildcardSelector & WILDMEMORIZESPECIFIC) 
				{
					int index = (wildcardSelector >> SPECIFIC_SHIFT) & 0x0000001f;
					SetWildCardGiven(positionStart,positionEnd,true,index);  // specific swallow 
					
					if (uppercasematch)
					{
						WORDP D = FindWord(wildcardOriginalText[index],0,UPPERCASE_LOOKUP); // find without underscores..
						if (D) 
						{
							strcpy(wildcardOriginalText[index],D->word);
							strcpy(wildcardCanonicalText[index],D->word);
						}
						else
						{
							char word[MAX_WORD_SIZE];
							strcpy(word,wildcardOriginalText[index]);
							char* at = word;
							while ((at = strchr(at,' '))) *at = '_';
							D = FindWord(word,0,UPPERCASE_LOOKUP); // find with underscores..
							if (D) 
							{
								strcpy(wildcardOriginalText[index],D->word);
								strcpy(wildcardCanonicalText[index],D->word);
							}
						}
						uppercasematch = false;
					}
					else if (strchr(wildcardCanonicalText[index],' ')) // is lower case canonical a dictionary word with content?
					{
						char word[MAX_WORD_SIZE];
						strcpy(word,wildcardCanonicalText[index]);
						char* at = word;
						while ((at = strchr(at,' '))) *at = '_';
						WORDP D = FindWord(word,0); // find without underscores..
						if (D && D->properties & PART_OF_SPEECH)  strcpy(wildcardCanonicalText[index],D->word);
					}
				}
			}
			wildcardSelector = 0; // completes all memorization at this level
		}
		else //   fix side effects of anything that failed to match by reverting
        {
            positionStart = oldStart;
            positionEnd = oldEnd;
  			if (*kind == '(' || *kind == '<') wildcardSelector = 0; /// should NOT clear this inside a [] or a {} on failure since they must try again
        }

        //   end sequence/choice/optional/random
        if (*word == ')' || *word ==  ']' || *word == '}' || (*word == '>' && word[1] == '>')) 
        {
			success = matched != 0; 
			if (success && argumentText) //   we are ok, but we need to resume old data
			{
				ptr = argumentText;
				argumentText = NULL;
				continue;
			}

			break;
        }

		//   postprocess match of single word or paren expression
		if (statusBits & NOT_BIT) //   flip failure result to success now (after wildcardsetting doesnt happen because formally match failed first)
        {
            matched = true; 
			statusBits &= -1 ^ (NOT_BIT|NOTNOT_BIT);
         }

		//   word ptr may not advance more than 1 at a time (allowed to advance 0 - like a string match or test) unless global unmarks in progress
        //   But if initial start was INFINITE_MATCH, allowed to match anywhere to start with
		if (legalgap || !matched  || positionStart == INFINITE_MATCH || oldStart == INFINITE_MATCH) {;}
		else if (reverse)
		{
			if (oldStart < oldEnd && positionEnd >= (oldStart - 1) ){;} // legal move ahead given matched WITHIN last time
			else if (positionEnd < (oldStart - 1 ))  // failed to match position advance
			{
				int ignored = oldStart - 1;
				if (oldStart && unmarked[ignored]) while (--ignored > positionEnd && unmarked[ignored]); // dont have to account for these
				if (ignored != positionStart) // position track failed
				{
					if (firstMatched == positionStart) firstMatched = 0; // drop recog of it
					matched = false;
					positionStart = oldStart;
					positionEnd = oldEnd;
				}
			}
		}
		else if (!legalgap && wildstart != 2) // forward requirement
		{
			if (oldEnd < oldStart && positionStart <= (oldStart + 1)){;} // legal move ahead given matched WITHIN last time -- what does match within mean?
			else if (positionStart > (oldEnd + 1))  // failed to match position advance of one
			{
				int ignored = oldEnd+1;
				if (unmarked[ignored]) while (++ignored < positionStart && unmarked[ignored]); // dont have to account for these
				if (ignored != positionStart) // position track failed
				{
					if (firstMatched == positionStart) firstMatched = 0; // drop recog of it
					matched = false;
					positionStart = oldStart;
					positionEnd = oldEnd;
				}
			}
		}
		if (*kind != '[' && *kind != '{') wildstart = 0; // now required to match in order here after
		
		if (trace & TRACE_PATTERN  && CheckTopicTrace()) 
		{
			bool success = matched;
			if (statusBits & NOT_BIT) success = !success;
			if (*word == '[' || *word == '{' || *word == '(' || (*word == '<' && word[1] == '<')) {} // seen on RETURN from a matching pair
			else if (*word == ']' || *word == '}' || *word == ')' || (*word == '>' && word[1] == '>')) {} 
			else
			{
				Log(STDUSERLOG,(char*)"%s",word);
				if (*word == '~' && matched) 
				{
					if (positionStart <= 0 || positionStart > wordCount || positionEnd <= 0 || positionEnd > wordCount) {;} // still in init startup?
					else if (positionStart != positionEnd) Log(STDUSERLOG,(char*)"(%s-%s)",wordStarts[positionStart],wordStarts[positionEnd]);
					else Log(STDUSERLOG,(char*)"(%s)",wordStarts[positionStart]);
				}
				else if (*word == '$' && matched) 
				{
					Log(STDUSERLOG,(char*)"(%s)",GetUserVariable(word));
				}
				else if (*word == '*' && matched && positionStart > 0 && positionStart <= wordCount && positionEnd <= wordCount) 
				{
					*word = 0;
					for (int i = positionStart; i <= positionEnd; ++i) 
					{
						if (*word) strcat(word,(char*)" ");
						strcat(word,wordStarts[i]);
					}
					Log(STDUSERLOG,(char*)"(%s)",word);
				}

				Log(STDUSERLOG,(success) ? (char*)"+ " : (char*)"- ");
			}
		}
	
        //   now verify position of match, NEXT is default for (type, not matter for others
        if (*kind == '(' || *kind == '<') //   ALL must match in sequence ( or jumbled <
        {
			//   we failed, retry shifting the start if we can
			if (!matched)
			{
				if (*kind == '<') break;	// we failed << >>

				if (wildstart == 1 && firstMatched > 0 && firstMatched < NORETRY) //   we are top level and have a first matcher, we can try to shift it
				{
					if (trace & TRACE_PATTERN  && CheckTopicTrace()) 
					{
						Log(STDUSERTABLOG,(char*)"------ Try pattern matching again, after word %d (%s) ------ ",firstMatched,wordStarts[firstMatched]);
						Log(STDUSERTABLOG,(char*)"");
					}
					//   reset to initial conditions, mostly 
					reverse = false;
					ptr = orig;
					wildcardIndex = 0; 
					basicStart = positionEnd = firstMatched;  //   THIS is different from inital conditions
					firstMatched = -1; 
					positionStart = INFINITE_MATCH; 
					wildcardSelector = 0;
					statusBits &= -1 ^ (NOT_BIT | FREEMODE_BIT | NOTNOT_BIT);
					argumentText = NULL; 
					// clear bit markers
					matchedBits[depth][0] = 0;
					matchedBits[depth][1] = 0;
					matchedBits[depth][2] = 0;
					matchedBits[depth][3] = 0;
					continue;
				}
				break; //   default fail
			}
			if (statusBits & FREEMODE_BIT) 
			{
				positionEnd = startposition;  //   allowed to pick up after here
				positionStart = INFINITE_MATCH; //   re-allow anywhere
			}
		}
        else if (matched) // was could not be END of sentence marker, why not???  
        {
			if (argumentText) //   we are ok, but we need to resume old data
			{
				ptr = argumentText;
				argumentText = NULL;
			}
			else if (*kind == '{' || *kind == '[')
			{
				success = true;
				break;	// we matched all needed in this
			}
		}
    } 

	//   begin the return sequence
	
	if (functionNest > startNest)//   since we are failing, we need to close out all function calls in progress at this level
    {
        callArgumentIndex = argStack[startNest];
        callArgumentBase = baseStack[startNest];
		fnVarBase = fnVarBaseStack[startNest];
		functionNest = startNest;
    }
	
	if (success)
	{
		returnstart = (firstMatched > 0) ? firstMatched : positionStart; // if never matched a real token, report 0 as start
		returnend = positionEnd;
	}

	//   if we leave this level w/o seeing the close, show it by elipsis 
	//   can only happen on [ and { via success and on ) by failure
	ChangeDepth(-1,(char*)"Match");
	globalDepth = startdepth; // insures even if we skip >> closes, we get correct depth
	if (trace & TRACE_PATTERN && depth  && CheckTopicTrace())
	{
		if (*word != ')' && *word != '}' && *word !=  ']' && (*word != '>' || word[1] != '>'))
		{
			if (*ptr != '}' && *ptr != ']' && *ptr != ')' && (*ptr != '>' || ptr[1] != '>')) Log(STDUSERLOG,(char*)"...");	// there is more in the pattern still
			if (success) 
			{
				if (*kind == '<') Log(STDUSERLOG,(char*)">>");
				else Log(STDUSERLOG,(char*)"%c",(*kind == '{') ? '}' : ']');
			}
			else if (*kind == '<') Log(STDUSERLOG,(char*)">>");
			else Log(STDUSERLOG,(char*)")");
		}
		else Log(STDUSERLOG,(char*)"%s",word); // we read to end of pattern 
		if (*word == '}') Log(STDUSERLOG,(char*)"+"); // optional always matches, by definition
		else Log(STDUSERLOG,(char*)"%c",matched ? '+' : '-');
		Log(STDUSERTABLOG,(char*)""); // next level resumed
	}
	if (trace & TRACE_PATTERN && !depth)
	{
		if (!matched)
		{
			char copy[MAX_WORD_SIZE];
			strncpy(copy,ptr,80);
			strcpy(copy+75,(char*)"...");
			char* at = strchr(copy,')');
			if (at) at[1] = 0;
			CleanOutput(copy);
			Log(STDUSERLOG,(char*)"        Remaining pattern: %s\r\n",copy);
		}
		else Log(STDUSERLOG,(char*)")+\r\n");
	}
   return success; 
}

