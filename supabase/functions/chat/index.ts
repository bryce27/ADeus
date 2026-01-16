import { serve } from "https://deno.land/std@0.170.0/http/server.ts";
import OpenAI from "https://deno.land/x/openai@v4.26.0/mod.ts";
import { corsHeaders } from "../common/cors.ts";
import { supabaseClient } from "../common/supabaseClient.ts";
import { ApplicationError, UserError } from "../common/errors.ts";

interface ChatClient {
  chat: {
    completions: {
      create: (params: { model: string; messages: Message[]; stream?: boolean }) => AsyncIterable<{ choices: Choice[] }>;
    };
  };
  embeddings: {
    create: (params: { model: string; input: string }) => Promise<{ data: any[] }>;
  };
}

// Enhanced search result with more context
interface SearchResult {
  id: number;
  raw_text: string;
  summary: string | null;
  topics: string[] | null;
  created_at: string;
  similarity: number;
  recency_score: number;
  combined_score: number;
}

interface Message {
  role: string;
  content: string;
}

interface Choice {
  delta: {
    content: string;
  };
}

// Detect time-related queries and extract time filter
function detectTimeFilter(query: string): number | null {
  const lowerQuery = query.toLowerCase();
  
  // Check for specific time patterns
  if (lowerQuery.includes("just now") || lowerQuery.includes("just said")) {
    return 1; // Last hour
  }
  if (lowerQuery.includes("today") || lowerQuery.includes("this morning") || lowerQuery.includes("this afternoon")) {
    return 24; // Last 24 hours
  }
  if (lowerQuery.includes("yesterday")) {
    return 48; // Last 48 hours
  }
  if (lowerQuery.includes("this week") || lowerQuery.includes("past week") || lowerQuery.includes("last few days")) {
    return 168; // Last 7 days
  }
  if (lowerQuery.includes("this month") || lowerQuery.includes("past month") || lowerQuery.includes("recently")) {
    return 720; // Last 30 days
  }
  if (lowerQuery.includes("last hour") || lowerQuery.includes("past hour")) {
    return 1;
  }
  
  // Match patterns like "last X hours/days"
  const hourMatch = lowerQuery.match(/last (\d+) hours?/);
  if (hourMatch) {
    return parseInt(hourMatch[1]);
  }
  
  const dayMatch = lowerQuery.match(/last (\d+) days?/);
  if (dayMatch) {
    return parseInt(dayMatch[1]) * 24;
  }
  
  return null; // No time filter detected
}

// Format a record for display in the prompt
function formatRecord(record: SearchResult): string {
  const date = new Date(record.created_at);
  const timeAgo = getTimeAgo(date);
  
  let formatted = `[${timeAgo}] ${record.raw_text}`;
  
  if (record.summary) {
    formatted += `\n  Summary: ${record.summary}`;
  }
  
  if (record.topics && record.topics.length > 0) {
    formatted += `\n  Topics: ${record.topics.join(", ")}`;
  }
  
  return formatted;
}

// Get human-readable time ago string
function getTimeAgo(date: Date): string {
  const now = new Date();
  const diffMs = now.getTime() - date.getTime();
  const diffMins = Math.floor(diffMs / 60000);
  const diffHours = Math.floor(diffMs / 3600000);
  const diffDays = Math.floor(diffMs / 86400000);
  
  if (diffMins < 1) return "just now";
  if (diffMins < 60) return `${diffMins} min ago`;
  if (diffHours < 24) return `${diffHours} hours ago`;
  if (diffDays === 1) return "yesterday";
  if (diffDays < 7) return `${diffDays} days ago`;
  if (diffDays < 30) return `${Math.floor(diffDays / 7)} weeks ago`;
  return date.toLocaleDateString();
}

// Current models available
type ModelName = "nousresearch/nous-capybara-34b" | "mistral" | "gpt-4-0125-preview";

const openaiClient = new OpenAI({
  apiKey: Deno.env.get("OPENAI_API_KEY"),
});
const useOpenRouter = Boolean(Deno.env.get("OPENROUTER_API_KEY"));
const useOllama = Boolean(Deno.env.get("OLLAMA_BASE_URL"));

async function* generateResponse(
  useOpenRouter: boolean,
  useOllama: boolean,
  messages: Message[]
) {
  let client: ChatClient;
  let modelName: ModelName;

  if (useOpenRouter) {
    client = new OpenAI({
      baseURL: "https://openrouter.ai/api/v1",
      apiKey: Deno.env.get("OPENROUTER_API_KEY"),
    });
    modelName = "nousresearch/nous-capybara-34b";
  } else if (useOllama) {
    client = new OpenAI({
      baseURL: Deno.env.get("OLLAMA_BASE_URL"),
      apiKey: "ollama"
    });
    modelName = "mistral"; 
  } else {
    client = openaiClient;
    modelName = "gpt-4-0125-preview";
  }

  const completion = await client.chat.completions.create({
    model: modelName,
    messages,
    stream: true,
  });

  for await (const chunk of completion) {
    yield chunk.choices[0].delta.content;
  }
}

async function getRelevantRecords(
  openaiClient: ChatClient,
  supabase: any, 
  messageHistory: Message[],
): Promise<SearchResult[]> {
  const lastMessage = messageHistory[messageHistory.length - 1].content;
  
  // Detect if this is a time-based query
  const timeFilterHours = detectTimeFilter(lastMessage);
  
  console.log(`Query: "${lastMessage}"`);
  console.log(`Time filter detected: ${timeFilterHours ? `${timeFilterHours} hours` : 'none'}`);
  
  // Embed the query using OpenAI's embeddings API
  const embeddingsResponse = await openaiClient.embeddings.create({
    model: "text-embedding-3-small",
    input: lastMessage,
  });

  const embeddings = embeddingsResponse.data[0].embedding;

  // Use the enhanced similarity search with time filtering
  const response = await supabase.rpc(
    "match_records_embeddings_similarity",
    {
      query_embedding: JSON.stringify(embeddings),
      match_threshold: 0.1,
      match_count: 15, // Get more results for better context
      time_filter_hours: timeFilterHours,
      user_auth_id: null, // Will use RLS
    }
  );

  if (response.error) {
    console.log("recordsError: ", response.error);
    // If the new function doesn't exist yet, fall back to the old one
    if (response.error.message?.includes("function") || response.error.code === "42883") {
      console.log("Falling back to legacy search function...");
      const legacyResponse = await supabase.rpc(
        "match_records_embeddings_similarity",
        {
          query_embedding: JSON.stringify(embeddings),
          match_threshold: 0.1,
          match_count: 10,
        }
      );
      if (legacyResponse.error) {
        throw new ApplicationError("Error getting records from Supabase");
      }
      return legacyResponse.data || [];
    }
    throw new ApplicationError("Error getting records from Supabase");
  }

  const relevantRecords: SearchResult[] = response.data || [];
  
  console.log(`Found ${relevantRecords.length} relevant records`);
  if (relevantRecords.length > 0) {
    console.log("Top result:", {
      similarity: relevantRecords[0].similarity,
      recency: relevantRecords[0].recency_score,
      combined: relevantRecords[0].combined_score,
      created_at: relevantRecords[0].created_at,
    });
  }

  return relevantRecords;
}

const chat = async (req: Request) => {

  if (req.method === "OPTIONS") {
    return new Response("ok", { headers: corsHeaders });
  }

  const supabaseAuthToken = req.headers.get("Authorization") ?? "";

  if (!supabaseAuthToken)
    throw new ApplicationError("Missing supabase auth token");
  
  const supabase = supabaseClient(req, supabaseAuthToken);

  const requestBody = await req.json();
  const msgData = requestBody as { messageHistory: Message[]; timestamp: string };

  if (!msgData.messageHistory) throw new UserError("Missing query in request data");
  const messageHistory = msgData.messageHistory;

  const relevantRecords = await getRelevantRecords(openaiClient, supabase, messageHistory);

  // Format records with timestamps and context
  const formattedRecords = relevantRecords.length > 0
    ? relevantRecords.map(formatRecord).join("\n\n")
    : "No relevant records found yet. The user hasn't recorded any memories matching this query.";

  // Extract unique topics for additional context
  const allTopics = relevantRecords
    .flatMap(r => r.topics || [])
    .filter((v, i, a) => a.indexOf(v) === i)
    .slice(0, 10);

  let messages = [
    {
      role: "system",
      content: `You are a highly personalized AI assistant with access to the user's recorded memories and conversations. 
You help them recall information, make connections between past discussions, and provide insights based on their personal context.

### Your Capabilities ###
- You have access to timestamped records of what the user has said and heard
- You can reference specific times ("you mentioned this yesterday", "a few hours ago you talked about...")
- You understand the context and topics from their past conversations
- You provide personalized, contextual responses

### Current Context ###
${allTopics.length > 0 ? `Recent topics the user has discussed: ${allTopics.join(", ")}` : ""}

### User's Relevant Memories ###
${formattedRecords}

### Response Guidelines ###
- Reference specific memories when relevant ("You mentioned earlier that...", "Based on what you said yesterday...")
- Be concise unless the user asks for details
- If the user asks about something not in their memories, acknowledge that and still try to help
- Use LaTeX formatting ($..$, $$..$$) for any mathematical expressions

### LaTeX Formatting ###
IMPORTANT: Wrap ALL mathematical expressions in LaTeX delimiters.
Available: $..$ for inline, $$...$$ for block, \\(...\\), \\[...\\]
`,
    },
    ...messageHistory,
  ];

  try {
    const stream = new ReadableStream({
      async start(controller) {
        try {
          const responseMessageGenerator = generateResponse(
            useOpenRouter,
            useOllama,
            messages
          );
  
          for await (const chunk of responseMessageGenerator) {
            const jsonResponse = JSON.stringify({ message: chunk }) + "\n";
            const encodedChunk = new TextEncoder().encode(jsonResponse);
            controller.enqueue(encodedChunk);
          }
          controller.close();
        } catch (error) {
          console.error("Stream error:", error);
          controller.error(error);
        }
      }
    });
  
    return new Response(stream, {
      headers: { ...corsHeaders, "Content-Type": "application/json" },
    });
  } catch (error) {
    console.log("Error: ", error);
    throw new ApplicationError("Error processing chat completion");
  }
};

serve(chat);
