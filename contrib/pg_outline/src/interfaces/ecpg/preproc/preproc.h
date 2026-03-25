/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_BASE_YY_PREPROC_H_INCLUDED
# define YY_BASE_YY_PREPROC_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int base_yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    SQL_ALLOCATE = 258,            /* SQL_ALLOCATE  */
    SQL_AUTOCOMMIT = 259,          /* SQL_AUTOCOMMIT  */
    SQL_BOOL = 260,                /* SQL_BOOL  */
    SQL_BREAK = 261,               /* SQL_BREAK  */
    SQL_CARDINALITY = 262,         /* SQL_CARDINALITY  */
    SQL_CONNECT = 263,             /* SQL_CONNECT  */
    SQL_COUNT = 264,               /* SQL_COUNT  */
    SQL_DATETIME_INTERVAL_CODE = 265, /* SQL_DATETIME_INTERVAL_CODE  */
    SQL_DATETIME_INTERVAL_PRECISION = 266, /* SQL_DATETIME_INTERVAL_PRECISION  */
    SQL_DESCRIBE = 267,            /* SQL_DESCRIBE  */
    SQL_DESCRIPTOR = 268,          /* SQL_DESCRIPTOR  */
    SQL_DISCONNECT = 269,          /* SQL_DISCONNECT  */
    SQL_FOUND = 270,               /* SQL_FOUND  */
    SQL_FREE = 271,                /* SQL_FREE  */
    SQL_GET = 272,                 /* SQL_GET  */
    SQL_GO = 273,                  /* SQL_GO  */
    SQL_GOTO = 274,                /* SQL_GOTO  */
    SQL_IDENTIFIED = 275,          /* SQL_IDENTIFIED  */
    SQL_INDICATOR = 276,           /* SQL_INDICATOR  */
    SQL_KEY_MEMBER = 277,          /* SQL_KEY_MEMBER  */
    SQL_LENGTH = 278,              /* SQL_LENGTH  */
    SQL_LONG = 279,                /* SQL_LONG  */
    SQL_NULLABLE = 280,            /* SQL_NULLABLE  */
    SQL_OCTET_LENGTH = 281,        /* SQL_OCTET_LENGTH  */
    SQL_OPEN = 282,                /* SQL_OPEN  */
    SQL_OUTPUT = 283,              /* SQL_OUTPUT  */
    SQL_REFERENCE = 284,           /* SQL_REFERENCE  */
    SQL_RETURNED_LENGTH = 285,     /* SQL_RETURNED_LENGTH  */
    SQL_RETURNED_OCTET_LENGTH = 286, /* SQL_RETURNED_OCTET_LENGTH  */
    SQL_SCALE = 287,               /* SQL_SCALE  */
    SQL_SECTION = 288,             /* SQL_SECTION  */
    SQL_SHORT = 289,               /* SQL_SHORT  */
    SQL_SIGNED = 290,              /* SQL_SIGNED  */
    SQL_SQLERROR = 291,            /* SQL_SQLERROR  */
    SQL_SQLPRINT = 292,            /* SQL_SQLPRINT  */
    SQL_SQLWARNING = 293,          /* SQL_SQLWARNING  */
    SQL_START = 294,               /* SQL_START  */
    SQL_STOP = 295,                /* SQL_STOP  */
    SQL_STRUCT = 296,              /* SQL_STRUCT  */
    SQL_UNSIGNED = 297,            /* SQL_UNSIGNED  */
    SQL_VAR = 298,                 /* SQL_VAR  */
    SQL_WHENEVER = 299,            /* SQL_WHENEVER  */
    S_ADD = 300,                   /* S_ADD  */
    S_AND = 301,                   /* S_AND  */
    S_ANYTHING = 302,              /* S_ANYTHING  */
    S_AUTO = 303,                  /* S_AUTO  */
    S_CONST = 304,                 /* S_CONST  */
    S_DEC = 305,                   /* S_DEC  */
    S_DIV = 306,                   /* S_DIV  */
    S_DOTPOINT = 307,              /* S_DOTPOINT  */
    S_EQUAL = 308,                 /* S_EQUAL  */
    S_EXTERN = 309,                /* S_EXTERN  */
    S_INC = 310,                   /* S_INC  */
    S_LSHIFT = 311,                /* S_LSHIFT  */
    S_MEMPOINT = 312,              /* S_MEMPOINT  */
    S_MEMBER = 313,                /* S_MEMBER  */
    S_MOD = 314,                   /* S_MOD  */
    S_MUL = 315,                   /* S_MUL  */
    S_NEQUAL = 316,                /* S_NEQUAL  */
    S_OR = 317,                    /* S_OR  */
    S_REGISTER = 318,              /* S_REGISTER  */
    S_RSHIFT = 319,                /* S_RSHIFT  */
    S_STATIC = 320,                /* S_STATIC  */
    S_SUB = 321,                   /* S_SUB  */
    S_VOLATILE = 322,              /* S_VOLATILE  */
    S_TYPEDEF = 323,               /* S_TYPEDEF  */
    CSTRING = 324,                 /* CSTRING  */
    CVARIABLE = 325,               /* CVARIABLE  */
    CPP_LINE = 326,                /* CPP_LINE  */
    IP = 327,                      /* IP  */
    DOLCONST = 328,                /* DOLCONST  */
    ECONST = 329,                  /* ECONST  */
    NCONST = 330,                  /* NCONST  */
    UCONST = 331,                  /* UCONST  */
    UIDENT = 332,                  /* UIDENT  */
    IDENT = 333,                   /* IDENT  */
    FCONST = 334,                  /* FCONST  */
    SCONST = 335,                  /* SCONST  */
    BCONST = 336,                  /* BCONST  */
    XCONST = 337,                  /* XCONST  */
    Op = 338,                      /* Op  */
    ICONST = 339,                  /* ICONST  */
    PARAM = 340,                   /* PARAM  */
    TYPECAST = 341,                /* TYPECAST  */
    DOT_DOT = 342,                 /* DOT_DOT  */
    COLON_EQUALS = 343,            /* COLON_EQUALS  */
    EQUALS_GREATER = 344,          /* EQUALS_GREATER  */
    LESS_EQUALS = 345,             /* LESS_EQUALS  */
    GREATER_EQUALS = 346,          /* GREATER_EQUALS  */
    NOT_EQUALS = 347,              /* NOT_EQUALS  */
    ABORT_P = 348,                 /* ABORT_P  */
    ABSOLUTE_P = 349,              /* ABSOLUTE_P  */
    ACCESS = 350,                  /* ACCESS  */
    ACTION = 351,                  /* ACTION  */
    ADD_P = 352,                   /* ADD_P  */
    ADMIN = 353,                   /* ADMIN  */
    AFTER = 354,                   /* AFTER  */
    AGGREGATE = 355,               /* AGGREGATE  */
    ALL = 356,                     /* ALL  */
    ALSO = 357,                    /* ALSO  */
    ALTER = 358,                   /* ALTER  */
    ALWAYS = 359,                  /* ALWAYS  */
    ANALYSE = 360,                 /* ANALYSE  */
    ANALYZE = 361,                 /* ANALYZE  */
    AND = 362,                     /* AND  */
    ANY = 363,                     /* ANY  */
    ARRAY = 364,                   /* ARRAY  */
    AS = 365,                      /* AS  */
    ASC = 366,                     /* ASC  */
    ASSERTION = 367,               /* ASSERTION  */
    ASSIGNMENT = 368,              /* ASSIGNMENT  */
    ASYMMETRIC = 369,              /* ASYMMETRIC  */
    AT = 370,                      /* AT  */
    ATTACH = 371,                  /* ATTACH  */
    ATTRIBUTE = 372,               /* ATTRIBUTE  */
    AUTHORIZATION = 373,           /* AUTHORIZATION  */
    BACKWARD = 374,                /* BACKWARD  */
    BEFORE = 375,                  /* BEFORE  */
    BEGIN_P = 376,                 /* BEGIN_P  */
    BETWEEN = 377,                 /* BETWEEN  */
    BIGINT = 378,                  /* BIGINT  */
    BINARY = 379,                  /* BINARY  */
    BIT = 380,                     /* BIT  */
    BOOLEAN_P = 381,               /* BOOLEAN_P  */
    BOTH = 382,                    /* BOTH  */
    BY = 383,                      /* BY  */
    CACHE = 384,                   /* CACHE  */
    CALL = 385,                    /* CALL  */
    CALLED = 386,                  /* CALLED  */
    CASCADE = 387,                 /* CASCADE  */
    CASCADED = 388,                /* CASCADED  */
    CASE = 389,                    /* CASE  */
    CAST = 390,                    /* CAST  */
    CATALOG_P = 391,               /* CATALOG_P  */
    CHAIN = 392,                   /* CHAIN  */
    CHAR_P = 393,                  /* CHAR_P  */
    CHARACTER = 394,               /* CHARACTER  */
    CHARACTERISTICS = 395,         /* CHARACTERISTICS  */
    CHECK = 396,                   /* CHECK  */
    CHECKPOINT = 397,              /* CHECKPOINT  */
    CLASS = 398,                   /* CLASS  */
    CLOSE = 399,                   /* CLOSE  */
    CLUSTER = 400,                 /* CLUSTER  */
    COALESCE = 401,                /* COALESCE  */
    COLLATE = 402,                 /* COLLATE  */
    COLLATION = 403,               /* COLLATION  */
    COLUMN = 404,                  /* COLUMN  */
    COLUMNS = 405,                 /* COLUMNS  */
    COMMENT = 406,                 /* COMMENT  */
    COMMENTS = 407,                /* COMMENTS  */
    COMMIT = 408,                  /* COMMIT  */
    COMMITTED = 409,               /* COMMITTED  */
    CONCURRENTLY = 410,            /* CONCURRENTLY  */
    CONFIGURATION = 411,           /* CONFIGURATION  */
    CONFLICT = 412,                /* CONFLICT  */
    CONNECTION = 413,              /* CONNECTION  */
    CONSTRAINT = 414,              /* CONSTRAINT  */
    CONSTRAINTS = 415,             /* CONSTRAINTS  */
    CONTENT_P = 416,               /* CONTENT_P  */
    CONTINUE_P = 417,              /* CONTINUE_P  */
    CONVERSION_P = 418,            /* CONVERSION_P  */
    COPY = 419,                    /* COPY  */
    COST = 420,                    /* COST  */
    CREATE = 421,                  /* CREATE  */
    CROSS = 422,                   /* CROSS  */
    CSV = 423,                     /* CSV  */
    CUBE = 424,                    /* CUBE  */
    CURRENT_P = 425,               /* CURRENT_P  */
    CURRENT_CATALOG = 426,         /* CURRENT_CATALOG  */
    CURRENT_DATE = 427,            /* CURRENT_DATE  */
    CURRENT_ROLE = 428,            /* CURRENT_ROLE  */
    CURRENT_SCHEMA = 429,          /* CURRENT_SCHEMA  */
    CURRENT_TIME = 430,            /* CURRENT_TIME  */
    CURRENT_TIMESTAMP = 431,       /* CURRENT_TIMESTAMP  */
    CURRENT_USER = 432,            /* CURRENT_USER  */
    CURSOR = 433,                  /* CURSOR  */
    CYCLE = 434,                   /* CYCLE  */
    DATA_P = 435,                  /* DATA_P  */
    DATABASE = 436,                /* DATABASE  */
    DAY_P = 437,                   /* DAY_P  */
    DEALLOCATE = 438,              /* DEALLOCATE  */
    DEC = 439,                     /* DEC  */
    DECIMAL_P = 440,               /* DECIMAL_P  */
    DECLARE = 441,                 /* DECLARE  */
    DEFAULT = 442,                 /* DEFAULT  */
    DEFAULTS = 443,                /* DEFAULTS  */
    DEFERRABLE = 444,              /* DEFERRABLE  */
    DEFERRED = 445,                /* DEFERRED  */
    DEFINER = 446,                 /* DEFINER  */
    DELETE_P = 447,                /* DELETE_P  */
    DELIMITER = 448,               /* DELIMITER  */
    DELIMITERS = 449,              /* DELIMITERS  */
    DEPENDS = 450,                 /* DEPENDS  */
    DESC = 451,                    /* DESC  */
    DETACH = 452,                  /* DETACH  */
    DICTIONARY = 453,              /* DICTIONARY  */
    DISABLE_P = 454,               /* DISABLE_P  */
    DISCARD = 455,                 /* DISCARD  */
    DISTINCT = 456,                /* DISTINCT  */
    DO = 457,                      /* DO  */
    DOCUMENT_P = 458,              /* DOCUMENT_P  */
    DOMAIN_P = 459,                /* DOMAIN_P  */
    DOUBLE_P = 460,                /* DOUBLE_P  */
    DROP = 461,                    /* DROP  */
    EACH = 462,                    /* EACH  */
    ELSE = 463,                    /* ELSE  */
    ENABLE_P = 464,                /* ENABLE_P  */
    ENCODING = 465,                /* ENCODING  */
    ENCRYPTED = 466,               /* ENCRYPTED  */
    END_P = 467,                   /* END_P  */
    ENUM_P = 468,                  /* ENUM_P  */
    ESCAPE = 469,                  /* ESCAPE  */
    EVENT = 470,                   /* EVENT  */
    EXCEPT = 471,                  /* EXCEPT  */
    EXCLUDE = 472,                 /* EXCLUDE  */
    EXCLUDING = 473,               /* EXCLUDING  */
    EXCLUSIVE = 474,               /* EXCLUSIVE  */
    EXECUTE = 475,                 /* EXECUTE  */
    EXISTS = 476,                  /* EXISTS  */
    EXPLAIN = 477,                 /* EXPLAIN  */
    EXTENSION = 478,               /* EXTENSION  */
    EXTERNAL = 479,                /* EXTERNAL  */
    EXTRACT = 480,                 /* EXTRACT  */
    FALSE_P = 481,                 /* FALSE_P  */
    FAMILY = 482,                  /* FAMILY  */
    FETCH = 483,                   /* FETCH  */
    FILTER = 484,                  /* FILTER  */
    FIRST_P = 485,                 /* FIRST_P  */
    FLOAT_P = 486,                 /* FLOAT_P  */
    FOLLOWING = 487,               /* FOLLOWING  */
    FOR = 488,                     /* FOR  */
    FORCE = 489,                   /* FORCE  */
    FOREIGN = 490,                 /* FOREIGN  */
    FORWARD = 491,                 /* FORWARD  */
    FREEZE = 492,                  /* FREEZE  */
    FROM = 493,                    /* FROM  */
    FULL = 494,                    /* FULL  */
    FUNCTION = 495,                /* FUNCTION  */
    FUNCTIONS = 496,               /* FUNCTIONS  */
    GENERATED = 497,               /* GENERATED  */
    GLOBAL = 498,                  /* GLOBAL  */
    GRANT = 499,                   /* GRANT  */
    GRANTED = 500,                 /* GRANTED  */
    GREATEST = 501,                /* GREATEST  */
    GROUP_P = 502,                 /* GROUP_P  */
    GROUPING = 503,                /* GROUPING  */
    GROUPS = 504,                  /* GROUPS  */
    HANDLER = 505,                 /* HANDLER  */
    HAVING = 506,                  /* HAVING  */
    HEADER_P = 507,                /* HEADER_P  */
    HOLD = 508,                    /* HOLD  */
    HOUR_P = 509,                  /* HOUR_P  */
    IDENTITY_P = 510,              /* IDENTITY_P  */
    IF_P = 511,                    /* IF_P  */
    ILIKE = 512,                   /* ILIKE  */
    IMMEDIATE = 513,               /* IMMEDIATE  */
    IMMUTABLE = 514,               /* IMMUTABLE  */
    IMPLICIT_P = 515,              /* IMPLICIT_P  */
    IMPORT_P = 516,                /* IMPORT_P  */
    IN_P = 517,                    /* IN_P  */
    INCLUDE = 518,                 /* INCLUDE  */
    INCLUDING = 519,               /* INCLUDING  */
    INCREMENT = 520,               /* INCREMENT  */
    INDEX = 521,                   /* INDEX  */
    INDEXES = 522,                 /* INDEXES  */
    INHERIT = 523,                 /* INHERIT  */
    INHERITS = 524,                /* INHERITS  */
    INITIALLY = 525,               /* INITIALLY  */
    INLINE_P = 526,                /* INLINE_P  */
    INNER_P = 527,                 /* INNER_P  */
    INOUT = 528,                   /* INOUT  */
    INPUT_P = 529,                 /* INPUT_P  */
    INSENSITIVE = 530,             /* INSENSITIVE  */
    INSERT = 531,                  /* INSERT  */
    INSTEAD = 532,                 /* INSTEAD  */
    INT_P = 533,                   /* INT_P  */
    INTEGER = 534,                 /* INTEGER  */
    INTERSECT = 535,               /* INTERSECT  */
    INTERVAL = 536,                /* INTERVAL  */
    INTO = 537,                    /* INTO  */
    INVOKER = 538,                 /* INVOKER  */
    IS = 539,                      /* IS  */
    ISNULL = 540,                  /* ISNULL  */
    ISOLATION = 541,               /* ISOLATION  */
    JOIN = 542,                    /* JOIN  */
    KEY = 543,                     /* KEY  */
    LABEL = 544,                   /* LABEL  */
    LANGUAGE = 545,                /* LANGUAGE  */
    LARGE_P = 546,                 /* LARGE_P  */
    LAST_P = 547,                  /* LAST_P  */
    LATERAL_P = 548,               /* LATERAL_P  */
    LEADING = 549,                 /* LEADING  */
    LEAKPROOF = 550,               /* LEAKPROOF  */
    LEAST = 551,                   /* LEAST  */
    LEFT = 552,                    /* LEFT  */
    LEVEL = 553,                   /* LEVEL  */
    LIKE = 554,                    /* LIKE  */
    LIMIT = 555,                   /* LIMIT  */
    LISTEN = 556,                  /* LISTEN  */
    LOAD = 557,                    /* LOAD  */
    LOCAL = 558,                   /* LOCAL  */
    LOCALTIME = 559,               /* LOCALTIME  */
    LOCALTIMESTAMP = 560,          /* LOCALTIMESTAMP  */
    LOCATION = 561,                /* LOCATION  */
    LOCK_P = 562,                  /* LOCK_P  */
    LOCKED = 563,                  /* LOCKED  */
    LOGGED = 564,                  /* LOGGED  */
    MAPPING = 565,                 /* MAPPING  */
    MATCH = 566,                   /* MATCH  */
    MATERIALIZED = 567,            /* MATERIALIZED  */
    MAXVALUE = 568,                /* MAXVALUE  */
    METHOD = 569,                  /* METHOD  */
    MINUTE_P = 570,                /* MINUTE_P  */
    MINVALUE = 571,                /* MINVALUE  */
    MODE = 572,                    /* MODE  */
    MONTH_P = 573,                 /* MONTH_P  */
    MOVE = 574,                    /* MOVE  */
    NAME_P = 575,                  /* NAME_P  */
    NAMES = 576,                   /* NAMES  */
    NATIONAL = 577,                /* NATIONAL  */
    NATURAL = 578,                 /* NATURAL  */
    NCHAR = 579,                   /* NCHAR  */
    NEW = 580,                     /* NEW  */
    NEXT = 581,                    /* NEXT  */
    NO = 582,                      /* NO  */
    NONE = 583,                    /* NONE  */
    NOT = 584,                     /* NOT  */
    NOTHING = 585,                 /* NOTHING  */
    NOTIFY = 586,                  /* NOTIFY  */
    NOTNULL = 587,                 /* NOTNULL  */
    NOWAIT = 588,                  /* NOWAIT  */
    NULL_P = 589,                  /* NULL_P  */
    NULLIF = 590,                  /* NULLIF  */
    NULLS_P = 591,                 /* NULLS_P  */
    NUMERIC = 592,                 /* NUMERIC  */
    OBJECT_P = 593,                /* OBJECT_P  */
    OF = 594,                      /* OF  */
    OFF = 595,                     /* OFF  */
    OFFSET = 596,                  /* OFFSET  */
    OIDS = 597,                    /* OIDS  */
    OLD = 598,                     /* OLD  */
    ON = 599,                      /* ON  */
    ONLY = 600,                    /* ONLY  */
    OPERATOR = 601,                /* OPERATOR  */
    OPTION = 602,                  /* OPTION  */
    OPTIONS = 603,                 /* OPTIONS  */
    OR = 604,                      /* OR  */
    ORDER = 605,                   /* ORDER  */
    ORDINALITY = 606,              /* ORDINALITY  */
    OTHERS = 607,                  /* OTHERS  */
    OUT_P = 608,                   /* OUT_P  */
    OUTER_P = 609,                 /* OUTER_P  */
    OVER = 610,                    /* OVER  */
    OVERLAPS = 611,                /* OVERLAPS  */
    OVERLAY = 612,                 /* OVERLAY  */
    OVERRIDING = 613,              /* OVERRIDING  */
    OWNED = 614,                   /* OWNED  */
    OWNER = 615,                   /* OWNER  */
    PARALLEL = 616,                /* PARALLEL  */
    PARSER = 617,                  /* PARSER  */
    PARTIAL = 618,                 /* PARTIAL  */
    PARTITION = 619,               /* PARTITION  */
    PASSING = 620,                 /* PASSING  */
    PASSWORD = 621,                /* PASSWORD  */
    PLACING = 622,                 /* PLACING  */
    PLANS = 623,                   /* PLANS  */
    POLICY = 624,                  /* POLICY  */
    POSITION = 625,                /* POSITION  */
    PRECEDING = 626,               /* PRECEDING  */
    PRECISION = 627,               /* PRECISION  */
    PRESERVE = 628,                /* PRESERVE  */
    PREPARE = 629,                 /* PREPARE  */
    PREPARED = 630,                /* PREPARED  */
    PRIMARY = 631,                 /* PRIMARY  */
    PRIOR = 632,                   /* PRIOR  */
    PRIVILEGES = 633,              /* PRIVILEGES  */
    PROCEDURAL = 634,              /* PROCEDURAL  */
    PROCEDURE = 635,               /* PROCEDURE  */
    PROCEDURES = 636,              /* PROCEDURES  */
    PROGRAM = 637,                 /* PROGRAM  */
    PUBLICATION = 638,             /* PUBLICATION  */
    QUOTE = 639,                   /* QUOTE  */
    RANGE = 640,                   /* RANGE  */
    READ = 641,                    /* READ  */
    REAL = 642,                    /* REAL  */
    REASSIGN = 643,                /* REASSIGN  */
    RECHECK = 644,                 /* RECHECK  */
    RECURSIVE = 645,               /* RECURSIVE  */
    REF_P = 646,                   /* REF_P  */
    REFERENCES = 647,              /* REFERENCES  */
    REFERENCING = 648,             /* REFERENCING  */
    REFRESH = 649,                 /* REFRESH  */
    REINDEX = 650,                 /* REINDEX  */
    RELATIVE_P = 651,              /* RELATIVE_P  */
    RELEASE = 652,                 /* RELEASE  */
    RENAME = 653,                  /* RENAME  */
    REPEATABLE = 654,              /* REPEATABLE  */
    REPLACE = 655,                 /* REPLACE  */
    REPLICA = 656,                 /* REPLICA  */
    RESET = 657,                   /* RESET  */
    RESTART = 658,                 /* RESTART  */
    RESTRICT = 659,                /* RESTRICT  */
    RETURNING = 660,               /* RETURNING  */
    RETURNS = 661,                 /* RETURNS  */
    REVOKE = 662,                  /* REVOKE  */
    RIGHT = 663,                   /* RIGHT  */
    ROLE = 664,                    /* ROLE  */
    ROLLBACK = 665,                /* ROLLBACK  */
    ROLLUP = 666,                  /* ROLLUP  */
    ROUTINE = 667,                 /* ROUTINE  */
    ROUTINES = 668,                /* ROUTINES  */
    ROW = 669,                     /* ROW  */
    ROWS = 670,                    /* ROWS  */
    RULE = 671,                    /* RULE  */
    SAVEPOINT = 672,               /* SAVEPOINT  */
    SCHEMA = 673,                  /* SCHEMA  */
    SCHEMAS = 674,                 /* SCHEMAS  */
    SCROLL = 675,                  /* SCROLL  */
    SEARCH = 676,                  /* SEARCH  */
    SECOND_P = 677,                /* SECOND_P  */
    SECURITY = 678,                /* SECURITY  */
    SELECT = 679,                  /* SELECT  */
    SEQUENCE = 680,                /* SEQUENCE  */
    SEQUENCES = 681,               /* SEQUENCES  */
    SERIALIZABLE = 682,            /* SERIALIZABLE  */
    SERVER = 683,                  /* SERVER  */
    SESSION = 684,                 /* SESSION  */
    SESSION_USER = 685,            /* SESSION_USER  */
    SET = 686,                     /* SET  */
    SETS = 687,                    /* SETS  */
    SETOF = 688,                   /* SETOF  */
    SHARE = 689,                   /* SHARE  */
    SHOW = 690,                    /* SHOW  */
    SIMILAR = 691,                 /* SIMILAR  */
    SIMPLE = 692,                  /* SIMPLE  */
    SKIP = 693,                    /* SKIP  */
    SMALLINT = 694,                /* SMALLINT  */
    SNAPSHOT = 695,                /* SNAPSHOT  */
    SOME = 696,                    /* SOME  */
    SQL_P = 697,                   /* SQL_P  */
    STABLE = 698,                  /* STABLE  */
    STANDALONE_P = 699,            /* STANDALONE_P  */
    START = 700,                   /* START  */
    STATEMENT = 701,               /* STATEMENT  */
    STATISTICS = 702,              /* STATISTICS  */
    STDIN = 703,                   /* STDIN  */
    STDOUT = 704,                  /* STDOUT  */
    STORAGE = 705,                 /* STORAGE  */
    STORED = 706,                  /* STORED  */
    STRICT_P = 707,                /* STRICT_P  */
    STRIP_P = 708,                 /* STRIP_P  */
    SUBSCRIPTION = 709,            /* SUBSCRIPTION  */
    SUBSTRING = 710,               /* SUBSTRING  */
    SUPPORT = 711,                 /* SUPPORT  */
    SYMMETRIC = 712,               /* SYMMETRIC  */
    SYSID = 713,                   /* SYSID  */
    SYSTEM_P = 714,                /* SYSTEM_P  */
    TABLE = 715,                   /* TABLE  */
    TABLES = 716,                  /* TABLES  */
    TABLESAMPLE = 717,             /* TABLESAMPLE  */
    TABLESPACE = 718,              /* TABLESPACE  */
    TEMP = 719,                    /* TEMP  */
    TEMPLATE = 720,                /* TEMPLATE  */
    TEMPORARY = 721,               /* TEMPORARY  */
    TEXT_P = 722,                  /* TEXT_P  */
    THEN = 723,                    /* THEN  */
    TIES = 724,                    /* TIES  */
    TIME = 725,                    /* TIME  */
    TIMESTAMP = 726,               /* TIMESTAMP  */
    TO = 727,                      /* TO  */
    TRAILING = 728,                /* TRAILING  */
    TRANSACTION = 729,             /* TRANSACTION  */
    TRANSFORM = 730,               /* TRANSFORM  */
    TREAT = 731,                   /* TREAT  */
    TRIGGER = 732,                 /* TRIGGER  */
    TRIM = 733,                    /* TRIM  */
    TRUE_P = 734,                  /* TRUE_P  */
    TRUNCATE = 735,                /* TRUNCATE  */
    TRUSTED = 736,                 /* TRUSTED  */
    TYPE_P = 737,                  /* TYPE_P  */
    TYPES_P = 738,                 /* TYPES_P  */
    UNBOUNDED = 739,               /* UNBOUNDED  */
    UNCOMMITTED = 740,             /* UNCOMMITTED  */
    UNENCRYPTED = 741,             /* UNENCRYPTED  */
    UNION = 742,                   /* UNION  */
    UNIQUE = 743,                  /* UNIQUE  */
    UNKNOWN = 744,                 /* UNKNOWN  */
    UNLISTEN = 745,                /* UNLISTEN  */
    UNLOGGED = 746,                /* UNLOGGED  */
    UNTIL = 747,                   /* UNTIL  */
    UPDATE = 748,                  /* UPDATE  */
    USER = 749,                    /* USER  */
    USING = 750,                   /* USING  */
    VACUUM = 751,                  /* VACUUM  */
    VALID = 752,                   /* VALID  */
    VALIDATE = 753,                /* VALIDATE  */
    VALIDATOR = 754,               /* VALIDATOR  */
    VALUE_P = 755,                 /* VALUE_P  */
    VALUES = 756,                  /* VALUES  */
    VARCHAR = 757,                 /* VARCHAR  */
    VARIADIC = 758,                /* VARIADIC  */
    VARYING = 759,                 /* VARYING  */
    VERBOSE = 760,                 /* VERBOSE  */
    VERSION_P = 761,               /* VERSION_P  */
    VIEW = 762,                    /* VIEW  */
    VIEWS = 763,                   /* VIEWS  */
    VOLATILE = 764,                /* VOLATILE  */
    WHEN = 765,                    /* WHEN  */
    WHERE = 766,                   /* WHERE  */
    WHITESPACE_P = 767,            /* WHITESPACE_P  */
    WINDOW = 768,                  /* WINDOW  */
    WITH = 769,                    /* WITH  */
    WITHIN = 770,                  /* WITHIN  */
    WITHOUT = 771,                 /* WITHOUT  */
    WORK = 772,                    /* WORK  */
    WRAPPER = 773,                 /* WRAPPER  */
    WRITE = 774,                   /* WRITE  */
    XML_P = 775,                   /* XML_P  */
    XMLATTRIBUTES = 776,           /* XMLATTRIBUTES  */
    XMLCONCAT = 777,               /* XMLCONCAT  */
    XMLELEMENT = 778,              /* XMLELEMENT  */
    XMLEXISTS = 779,               /* XMLEXISTS  */
    XMLFOREST = 780,               /* XMLFOREST  */
    XMLNAMESPACES = 781,           /* XMLNAMESPACES  */
    XMLPARSE = 782,                /* XMLPARSE  */
    XMLPI = 783,                   /* XMLPI  */
    XMLROOT = 784,                 /* XMLROOT  */
    XMLSERIALIZE = 785,            /* XMLSERIALIZE  */
    XMLTABLE = 786,                /* XMLTABLE  */
    YEAR_P = 787,                  /* YEAR_P  */
    YES_P = 788,                   /* YES_P  */
    ZONE = 789,                    /* ZONE  */
    NOT_LA = 790,                  /* NOT_LA  */
    NULLS_LA = 791,                /* NULLS_LA  */
    WITH_LA = 792,                 /* WITH_LA  */
    POSTFIXOP = 793,               /* POSTFIXOP  */
    UMINUS = 794                   /* UMINUS  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 586 "preproc.y"

	double	dval;
	char	*str;
	int		ival;
	struct	when		action;
	struct	index		index;
	int		tagname;
	struct	this_type	type;
	enum	ECPGttype	type_enum;
	enum	ECPGdtype	dtype_enum;
	struct	fetch_desc	descriptor;
	struct  su_symbol	struct_union;
	struct	prep		prep;
	struct	exec		exec;

#line 619 "preproc.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif


extern YYSTYPE base_yylval;
extern YYLTYPE base_yylloc;

int base_yyparse (void);


#endif /* !YY_BASE_YY_PREPROC_H_INCLUDED  */
