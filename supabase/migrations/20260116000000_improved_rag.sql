-- Improved RAG: Enhanced similarity search with time filtering and recency weighting

-- First, ensure we're using the vector extension from the right schema
SET search_path TO public, extensions;

-- Drop old function if exists (with old signature)
DROP FUNCTION IF EXISTS public.match_records_embeddings_similarity(extensions.vector, double precision, integer);

-- New enhanced function with time filtering and more fields returned
CREATE OR REPLACE FUNCTION public.match_records_embeddings_similarity(
    query_embedding text,  -- Accept as text/JSON, cast internally
    match_threshold double precision,
    match_count integer,
    time_filter_hours integer DEFAULT NULL,
    user_auth_id uuid DEFAULT NULL
)
RETURNS TABLE(
    id integer,
    raw_text text,
    summary text,
    topics text[],
    created_at timestamp with time zone,
    similarity double precision,
    recency_score double precision,
    combined_score double precision
)
LANGUAGE plpgsql STABLE
AS $$
DECLARE
    query_vec extensions.vector;
BEGIN
    -- Parse the JSON array into a vector
    query_vec := query_embedding::extensions.vector;
    
    RETURN QUERY
    SELECT
        r.id::integer,
        r.raw_text,
        r.summary,
        r.topics,
        r.created_at,
        (1 - (r.embeddings::extensions.vector <=> query_vec))::double precision as similarity,
        -- Recency score: exponential decay, half-life of 7 days
        EXP(-0.099 * EXTRACT(EPOCH FROM (NOW() - r.created_at)) / 86400)::double precision as recency_score,
        -- Combined score: 70% similarity + 30% recency
        ((0.7 * (1 - (r.embeddings::extensions.vector <=> query_vec))) + 
        (0.3 * EXP(-0.099 * EXTRACT(EPOCH FROM (NOW() - r.created_at)) / 86400)))::double precision as combined_score
    FROM public.records r
    WHERE 
        -- Similarity threshold
        (1 - (r.embeddings::extensions.vector <=> query_vec)) > match_threshold
        -- Optional time filter
        AND (time_filter_hours IS NULL OR r.created_at > NOW() - (time_filter_hours || ' hours')::interval)
        -- Optional user filter (for RLS bypass in edge functions)
        AND (user_auth_id IS NULL OR r.auth_id = user_auth_id)
        -- Only processed records with embeddings
        AND r.embeddings IS NOT NULL
    ORDER BY combined_score DESC
    LIMIT match_count;
END;
$$;

-- Grant permissions
GRANT EXECUTE ON FUNCTION public.match_records_embeddings_similarity(
    text, double precision, integer, integer, uuid
) TO anon, authenticated, service_role;

-- New function: Search by topic
CREATE OR REPLACE FUNCTION public.search_records_by_topic(
    search_topic text,
    match_count integer DEFAULT 10,
    user_auth_id uuid DEFAULT NULL
)
RETURNS TABLE(
    id integer,
    raw_text text,
    summary text,
    topics text[],
    created_at timestamp with time zone
)
LANGUAGE sql STABLE
AS $$
    SELECT
        records.id::integer,
        records.raw_text,
        records.summary,
        records.topics,
        records.created_at
    FROM public.records
    WHERE 
        search_topic = ANY(records.topics)
        AND (user_auth_id IS NULL OR records.auth_id = user_auth_id)
    ORDER BY records.created_at DESC
    LIMIT match_count;
$$;

GRANT EXECUTE ON FUNCTION public.search_records_by_topic(text, integer, uuid) TO anon, authenticated, service_role;

-- New function: Get recent records (for "what did I just say?" queries)
CREATE OR REPLACE FUNCTION public.get_recent_records(
    hours_back integer DEFAULT 24,
    match_count integer DEFAULT 20,
    user_auth_id uuid DEFAULT NULL
)
RETURNS TABLE(
    id integer,
    raw_text text,
    summary text,
    topics text[],
    created_at timestamp with time zone
)
LANGUAGE sql STABLE
AS $$
    SELECT
        records.id::integer,
        records.raw_text,
        records.summary,
        records.topics,
        records.created_at
    FROM public.records
    WHERE 
        records.created_at > NOW() - (hours_back || ' hours')::interval
        AND (user_auth_id IS NULL OR records.auth_id = user_auth_id)
    ORDER BY records.created_at DESC
    LIMIT match_count;
$$;

GRANT EXECUTE ON FUNCTION public.get_recent_records(integer, integer, uuid) TO anon, authenticated, service_role;

-- Add index for faster topic searches (if not exists)
CREATE INDEX IF NOT EXISTS idx_records_topics ON public.records USING GIN (topics);

-- Add index for faster time-based queries (if not exists)
CREATE INDEX IF NOT EXISTS idx_records_created_at ON public.records (created_at DESC);
