#include "Player.hh"

#define PLAYER_NAME Eldar

/* Quick explanation of what the AI does: 
   This AI has (almost) no memory: at every round it recalculates the board state and performs the currently best action.
   - First, build_board creates an internal representation of the board state. This representation is stored in:
     - Matrices: Board (general info), Board_Enemy (HP and reachable cells of enemies) and Board_Barricades (HP of barricades)
     - Maps: For each cell with a weapon or money, store distance to closest enemy in this round AND ALSO LAST ROUND (only piece of memory of this AI)
   - Each citizen then computes its distance in turns to every cell by performing Dijkstra's algorithm on the Board matrix.
     When an object (bonus or enemy) is found, the true profit of the cell is calcultated as roughly (PROFIT_OF_OBJECT - distance).
     Some additional optimizations are performed:
     - If a move is obviously good (picking up a weapon or killing an enemy), it's taken direcltly instead of searching the board
     - A citizen will not try to get a weapon when another warrior (interested in the weapon) is closer, since it wouldn't get there in time.
     - Same with money, but assume that all citizens are interested (not just weak warriors). Also assume that, when a citizen is not DIRECTLY
       approaching money, it's not interested in grabbing it (maybe it has found a weapon or food, it's safe to go for the money).
   - Warriors always move to approach the target with highest profit. Builders at day, if the profit is under a theshold, attempt to build a barricade to
     make surviving at night easier. 
   - Movement is only performed if it's safe to do so, in order to minimize damage taken.
   - Once a citizen has decided what to do, the instruction gets stored in a priority queue that gets flushed at the end of the round.
     This allows assigning a priority to the instructions, since they get excecuted in the order they are sent.

   Some bookmarks:
   Line 191: build_board function and its auxiliary functions
   Line 354: approach_target function and its auxiliary functions (basically the main AI)
   Line 760: main function that calls the tasks of each citizen
   Line 109: bug that reduced my life expectancy by 2 years
*/

struct PLAYER_NAME : public Player {

    // Do not modify this function.
    static Player* factory() {
        return new PLAYER_NAME;
    }

    /* TYPES AND ATTRIBUTES */
    
    typedef vector<vector<unsigned short int>> Unsigned_Matrix;

    // The Judge assigns the players plenty of memory, so this isn't strictly needed. However it might improve cache performance,
    // since random accesses to the Board matrix are common.
    typedef vector<vector<char>> Small_Matrix;

    typedef vector<vector<int>> Matrix;
    
    const vector<Dir> Directions = {Up, Down, Left, Right};
    
    // Elements of the board, ordered from most to least beneficial
    const char BAZOOKA = 6;
    const char GUN = 5;
    const char HAMMER = 4;
    const char BUILDER = 3;
    const char FOOD = 2;
    const char MONEY = 1;
    const char EMPTY = 0;
    const char FRIENDLY_CITIZEN = -1;
    const char WALL = -2;
    const char ENEMY_BUILDER = -3; // All enemy warriors must be smaller than ENEMY_BUILDER
    const char ENEMY_HAMMER = -4;
    const char ENEMY_GUN = -5;
    const char ENEMY_BAZOOKA = -6;
    
    // Board itself
    int sz_n = 0, sz_m = 0;
    // Stores basic information about the contents of each cell
    Small_Matrix Board;
    // Stores extended information about the enemies in the board: >0 = life of that enemy,
    // ENEMY_HAMMER>val>ENEMY_BAZOOKA = enemy on a cell next to this (going there can cause you to take damage)
    Small_Matrix Board_Enemy;
    // Stores the barricades: 0 = no barricade, >0 = resistance of my barricade, <0 = -(resistance of enemy barricade)
    Matrix Board_Barricades;

    // Stores the amount of built barricades
    int num_barricades;
    
    // Representation of a player instruction that has been added to the buffer
    struct Instr {
        int priority;   // Instructions will be executed in order of priority instead of computation
        bool is_build;  // True if it's a "Build" instruction. False if it's a "Move" instruction
        int id;     // Citizen that performs the instruction
        Dir dir;    // Direction of movement/construction
        
        bool operator<(const Instr& i) const {
            if (priority == i.priority) return id < i.id;
            return priority < i.priority;
        }
    };
    
    // Priorities for some common actions: those indicate the order in which instructions are sent
    const int NOT_IMPORTANT = -1;       // priority doesn't matter
    const int BUILD_PRIORITY = 0;       // building barricade
    const int RUN_PRIORITY = 15;        // escaping (+1 hit away from dying)
    const int RUN_DEATH_PRIORITY = 20;  // escaping (1 hit away from dying)
    const int VERY_HIGH_PRIORITY = 500; // grabbing weapon or killing enemy
    
    // Queue for all instructions that need be executed. Allows assigning a priority to each instruction
    priority_queue<Instr> instruction_buffer;
    

    // Vertex representaton for Dijksta's algorithm
    struct Vertex {
        int dist;   // Distance from origin
        Pos pos;    // Position on the board
        Dir dir;    // Which direction should we move to reach the positin
        
        // An element is LESS ATTRACTIVE if it's distance is GREATER.
        bool operator<(const Vertex& i) const {
            return dist > i.dist; // This single '>' character is the reason I had to install a debugger
        }
    };

    // Information about closest citizens to a bonus
    struct Bonus_Info {
        char type = 'U'; // 'U' = Undefined, 'M' = Money, 'W' = Weapon
        // Money: store info about closest citizen (any type)
        // Weapons: store info about closest warrior that's strictly weaker than the weapon
        int closest_dist = 0;
        bool closest_is_friendly = 0;

        Bonus_Info() {}
        Bonus_Info(char c) { type = c; }
    };

    map<Pos, Bonus_Info>* bonus_distances; // Stores the distances from bonuses to their closest citizens. 
    map<Pos, Bonus_Info>* bonus_distances_prev; // Stores the contents of bonus_distances from last round

    // If bonus_distances and bonus_distances_prev were maps, at each round we would destroy 2 maps, copy 1 map and build 1 map.
    // Instead, we use pointers and toggle between 2 maps, so at each round we only destroy 1 map and build 1 map.
    map<Pos, Bonus_Info> bonus_distances_odd;
    map<Pos, Bonus_Info> bonus_distances_even;


    // Profits for cell contents: those are extremely important to tune correctly
    const int MONEY_PROFIT = 12;
    const int HEALTH_PROFIT = 17;
    const int ABOUT_TO_DIE_BONUS = 5;   // If unit is 1 hit away from dying, increase profit of health
    const int ATTACK_PROFIT = 19;       // Remaining hit points will be subtracted
    const int WEAPON_PROFIT = 25;
    const int STEAL_WEAPON_PROFIT = 12;
    const int BAZOOKA_EXTRA_PROFIT = 3; // Add 3 to WEAPON_PROFIT when stealing a bazooka and 6 when trying to get it
    const int WARRIOR_EXTRA_PROFIT = 3; // Increase profit for attacking a warior instead of a builder
    
    // Parameters for building barricades:
    const int BARRICADE_THRESHOLD = 2;              // Barricade threshold during the day
    //const int BUILDING_ROUNDS_NIGHT = 5;          // How many rounds before night to start building most barricades
    //const int BARRICADE_THRESHOLD_NIGHT = 2;      // Barricade threshold when it's about to be night (BUILDING_ROUNDS_NIGHT)
    const int BARRICADE_INTERRUPT_THRESHOLD = 5;    // Barricade threshold when currently building barricade
    const int PERCENT_BUILD = 70;                   // Barricades only get partially upgraded

    // Adding a small penalty to going to a cell with one of my units may reduce the cases where the AI gets stuck on itself
    const int COST_WALK_INTO_FRIENDLY = 3;



    /* INLINE FUNCTIONS for improving readability of common actions: they get get inserted inline without performance penalty */
    
    // Add a "move" instruction to the instruction buffer 
    inline void _move(int priority, int id, const Dir& where) {
        instruction_buffer.push({priority, false, id, where});
    }
    
    // Add a "build" instruction to the instruction buffer 
    inline void _build(int priority, int id, const Dir& where) {
        instruction_buffer.push({priority, true, id, where});
    }
    
    // Return the contents of the board at a given Dir
    inline char board(const Pos& p) {
        return Board[p.i][p.j];
    }
    // Return the contents of the enemy board at a given Dir
    inline char board_enemy(const Pos& p) {
        return Board_Enemy[p.i][p.j];
    }
    // Return the contents of the barricade board at a given Dir
    inline int board_barricades(const Pos& p) {
        return Board_Barricades[p.i][p.j];
    }
    
    // Returns the integer encoding of the weapon that "cit" carries
    inline char my_weapon(const Citizen& cit) {
        if (cit.weapon == NoWeapon) return BUILDER;
        else if (cit.weapon == Hammer) return HAMMER;
        else if (cit.weapon == Gun) return GUN;
        else return BAZOOKA; // cit.weapon == Bazooka
    }


    
    /* BUILD BOARD: This method gets called once at the beginning of each round. Its job is to extract the board contents */
    /*    at each cell and store them into an easy to traverse matrix. This speeds up the computation of each citizen.    */

    // Initialize board and sizes. Called only on first round.
    void initialize() {
        sz_n = board_rows();
        sz_m = board_cols();
        Board = Small_Matrix(sz_n, vector<char>(sz_m));
        Board_Enemy = Small_Matrix(sz_n, vector<char>(sz_m));
        Board_Barricades = Matrix(sz_n, vector<int>(sz_m));
    }


    // Simplified version of approach_target, used for computing the closest citizen to a given bonus
    void compute_closest(const Pos& origin, Bonus_Info& info) {
        // Store the distance from each position to the origin (starts at infinity)
        Unsigned_Matrix dist(sz_n, vector<unsigned short int>(sz_m, USHRT_MAX));
        // Store whether a position has been visited
        vector<vector<bool>> visited(sz_n, vector<bool>(sz_m, false));
        // Store pending vertices in order. Vertices may be repeated, they only get treated once (when their distance is infinity)
        priority_queue<Vertex> Q;

        int WEAPON = board(origin);

        // Enqueue initial position
        dist[origin.i][origin.j] = 0;
        Q.push({0, origin});

        // Dequeue until priority queue is empty
        while (not Q.empty()) {
            Pos u = Q.top().pos;    // Dequeue position u
            Q.pop();
            if (visited[u.i][u.j]) continue; // If the edge has already been visited, don't do anything else
            visited[u.i][u.j] = true; // Mark edge as visited
            int distance = dist[u.i][u.j];
            
            //  u contains an enemy or a friendly citizen
            if ((board(u) <= ENEMY_BUILDER or board(u) == FRIENDLY_CITIZEN)) {
                if (info.type == 'M') { // The bonus is money: assume everyone is interested
                    info.closest_is_friendly = (board(u) == FRIENDLY_CITIZEN);
                    info.closest_dist = distance;
                    // cerr << "Money at " << origin << ": dist=" << distance << ", friend=" << info.closest_is_friendly;
                    // cerr << ". Last round was: dist=" << (*bonus_distances_prev)[origin].closest_dist << ", friend=" << (*bonus_distances_prev)[origin].closest_is_friendly << endl;
                    return;
                }
                else if (info.type == 'W') {
                    // The bonus is a weapon: assume only weaker citizens are interested
                    if (board(u) == FRIENDLY_CITIZEN) {
                        // Found one of my citizens, check if it's a warrior and weaker than the weapon
                        Citizen c = citizen(cell(u).id);
                        if (c.type == Warrior and my_weapon(c) < WEAPON) {
                            info.closest_is_friendly = true;
                            info.closest_dist = distance;
                            // cerr << "Weapon at " << origin << ": dist=" << distance << ", friend=" << info.closest_is_friendly;
                            // cerr << ". Last round was: dist=" << (*bonus_distances_prev)[origin].closest_dist << ", friend=" << (*bonus_distances_prev)[origin].closest_is_friendly << endl;
                            return;
                        }
                    }
                    else if (board(u) < ENEMY_BUILDER and -board(u) < WEAPON) {
                        // Found an enemy that's weaker than the weapon
                        info.closest_is_friendly = false;
                        info.closest_dist = distance;
                        // cerr << "Weapon at " << origin << ": dist=" << distance << ", friend=" << info.closest_is_friendly;
                        // cerr << ". Last round was: dist=" << (*bonus_distances_prev)[origin].closest_dist << ", friend=" << (*bonus_distances_prev)[origin].closest_is_friendly << endl;
                        return; 
                    }
                }
            }
            
            // Keep searching on adjacent positions
            for (Dir d : Directions) {
                Pos new_p = u + d;
                // only add to queue if: inside the board, and not a wall, and...
                if (pos_ok(new_p) and board(new_p) != WALL) {
                    unsigned short int new_distance = dist[u.i][u.j] + 1; // By default, the cost in turns is 1
                    // Simplified version of add_movement_penalties, asume all citizens are bazookas and barricades affect everyone
                    if (board_barricades(new_p) < 0) distance += (-board_barricades(new_p)/bazooka_strength_demolish());
                    if (board_barricades(new_p) > 0) distance += (board_barricades(new_p)/bazooka_strength_demolish());

                    // ...and distance can be improved
                    if (new_distance < dist[new_p.i][new_p.j]) {
                        dist[new_p.i][new_p.j] = new_distance; // Mark new pos as visited by updating its distance to origin
                        Q.push({new_distance, new_p}); // Push new pos and propagate dir
                    }
                }
            }
        }
        // This point will almost never be reached: there isn't any citizen interested in this bonus (use default values)
    }

    // Given a cell of the Board_Enemy, update the danger factor of the cell
    inline void update_danger(char& cell, char enemy) {
        // If the cell contains the life of an enemy or it's the danger zone of a more powerful one, don't do anything
        if (cell > 0 or cell <= enemy) return;
        cell = enemy; // Else update cell value
    }
    
    // Build matrix representation of current board. Called once every round.
    void build_board() {
        bonus_distances->clear(); // Empty the distances to bonuses

        // Initialize Board_Enemy to 0;
        for (int i = 0; i < sz_n; ++i)
            for (int j = 0; j < sz_m; ++j)
                Board_Enemy[i][j] = 0;

        // Visit all cells and update Board matrices
        for (int i = 0; i < sz_n; ++i) {
            for (int j = 0; j < sz_m; ++j) {
                Cell c = cell(i, j);
                if (c.type == Building) Board[i][j] = WALL;
                else if (c.bonus == Food) Board[i][j] = FOOD;
                else if (c.bonus == Money) {
                    Board[i][j] = MONEY;
                    bonus_distances->insert({Pos(i,j), Bonus_Info('M')}); // "Enqueue" money cell
                }
                else if (c.weapon == Gun) {
                    Board[i][j] = GUN;
                    bonus_distances->insert({Pos(i,j), Bonus_Info('W')}); // "Enqueue" weapon cell
                }
                else if (c.weapon == Bazooka) {
                    Board[i][j] = BAZOOKA;
                    bonus_distances->insert({Pos(i,j), Bonus_Info('W')}); // "Enqueue" weapon cell
                }
                else if (c.id != -1) {
                    Citizen cit = citizen(c.id);
                    if (cit.player == me()) Board[i][j] = FRIENDLY_CITIZEN;
                    else {
                        char enemy;
                        if (cit.type == Builder) enemy = ENEMY_BUILDER;
                        else if (cit.weapon == Hammer) enemy = ENEMY_HAMMER;
                        else if (cit.weapon == Gun) enemy = ENEMY_GUN;
                        else /*if (cit.weapon == Bazooka)*/ enemy = ENEMY_BAZOOKA;
                        
                        Board[i][j] = enemy;
                        Board_Enemy[i][j] = cit.life;
                        
                        // For each adjacent position, update the danger zones
                        if (pos_ok(i-1, j)) update_danger(Board_Enemy[i-1][j], enemy);
                        if (pos_ok(i+1, j)) update_danger(Board_Enemy[i+1][j], enemy);
                        if (pos_ok(i, j-1)) update_danger(Board_Enemy[i][j-1], enemy);
                        if (pos_ok(i, j+1)) update_danger(Board_Enemy[i][j+1], enemy);
                    }
                }
                else Board[i][j] = EMPTY;   // Nothing of interest

                // Also see if there are barricades
                if (c.resistance == -1) Board_Barricades[i][j] = 0;
                else {
                    if (c.b_owner == me()) Board_Barricades[i][j] = c.resistance; // Store resistance of my barricade
                    else Board_Barricades[i][j] = -c.resistance; // Store resistance of enemy barricade
                }
            }
        }

        // Compute closest citizens for bonuses
        map<Pos,Bonus_Info>::iterator it;
        for (it = bonus_distances->begin(); it != bonus_distances->end(); ++it)
            compute_closest(it->first, it->second);
    }



    /*  APPROACH TARGET: This is the main algorithm that decides the movement of all units by implementing  */
    /* Dijksta's algorithm on the board: the edge weights are the cost in turns to go from a Pos to another */
    
    // Returns true is my citizen "cit" is equal or stronger than ENEMY_BUILDER <= Board[i][j] <= ENEMY_BAZOOKA
    inline bool is_stronger(const Citizen& cit, int i, int j) {
        char weapon = my_weapon(cit);
        // If both units have same weapon, the stroger one is the one with most life
        if (weapon == -Board[i][j]) return cit.life > citizen(cell(i, j).id).life;
        return weapon > -Board[i][j];
    }

    // Returns the damage that a given weapon does against a barricade
    inline int strength_demolish(char weapon) {
        if (weapon == BUILDER) return builder_strength_demolish();
        if (weapon == HAMMER) return hammer_strength_demolish();
        if (weapon == GUN) return gun_strength_demolish();
        return bazooka_strength_demolish(); // weapon == BAZOOKA
    }
    
    // Returns true if the position contains a barricade that can be improved
    inline bool has_buildable_barricade(Pos pos) {
        //   inside board   and   contains barricade      and    the barricade is empty      and  barricade can be improved (up to PERCENT_BUILD %)
        return (pos_ok(pos) and board_barricades(pos) > 0 and board(pos) != FRIENDLY_CITIZEN and  board_barricades(pos) < barricade_max_resistance()*PERCENT_BUILD/100);
    }

    // Returns true if going to this position will cause me to take damage
    inline bool is_danger(const Pos& pos, char my_weapon) {
        // If now is day (and next round will also be day), I won't take damage
        if (is_day() and is_round_day(round()+1)) return false;
        return -board_enemy(pos) > my_weapon;
    }

    // Returns true if going to this position is an obvious good choice: collect better weapon or kill weaker enemy
    inline bool no_brainer(int i, int j, char& my_weapon, bool is_warrior) {
        // weaker enemy (or equal) and about to die and can be attacked (is night and not inside a barricade)
        if (Board[i][j] <= ENEMY_BUILDER and -Board[i][j] <= my_weapon and is_night()
            and Board_Enemy[i][j] <= life_lost_in_attack() and Board_Barricades[i][j] == 0)
            return true;
        if (is_warrior and Board[i][j] > my_weapon) {
            my_weapon = Board[i][j]; // Imagine you already grabbed the new weapon
            return true; // better weapon
        }
        return false;
    }

    // Adds to distance the additional cost in turns to move to pos
    inline void add_movement_penalties(Pos pos, unsigned short int& distance, char weapon) {
        // Board contains enemy barricade, add cost to destroy it (-board_barricades(pos) = barricade health)
        if (board_barricades(pos) < 0) distance += (-board_barricades(pos)/strength_demolish(weapon));
        // Board contains enemy, add best case scenario cost (every hit causes damage) (board_enemy(pos) = enemy health)
        // Subtract 1 because you don't need to be in the same cell in order to attack (attack from adjacent cell)
        if (board(pos) <= ENEMY_BUILDER) distance += (board_enemy(pos)/life_lost_in_attack() - 1);
        // Board contains one of my units, adding a small penalty may reduce the cases where the AI gets stuck on itself
        if (board(pos) == FRIENDLY_CITIZEN) distance += COST_WALK_INTO_FRIENDLY;
    }
    
    // Given a citizen with ID,
    // - if a good direction is found (in order to approach the best point of interest), go there and return true
    // - if no good direction is found, return false
    bool approach_target(int ID, bool is_warrior) {
        // Store the distance from each position to the origin (starts at infinity)
        Unsigned_Matrix dist(sz_n, vector<unsigned short int>(sz_m, USHRT_MAX));
        // Store whether a position has been visited
        vector<vector<bool>> visited(sz_n, vector<bool>(sz_m, false));
        // Store pending vertices in order. Vertices may be repeated, they only get treated once (when their distance is infinity)
        priority_queue<Vertex> Q;
        Dir best_dir = Left;        // Direction to reach best vertex (Left is just a placeholder)
        int best_profit = INT_MIN;  // Profit of the best vertex (starts at -infinity)

        Bonus_Info* take_ownership = nullptr; // Pointer to Bonus_Info of the target weapon (if any)
        int take_ownership_dist = 0; // Distance to the target weapon (if any)

        Citizen c = citizen(ID);
        Pos origin = c.pos;

        // Compute whether we will take food if we find it
        bool NEED_HEAL;
        if (is_warrior) NEED_HEAL = c.life < warrior_ini_life();
        else NEED_HEAL = c.life < builder_ini_life();
        
        // Get the weapon I'm currently holding
        char WEAPON = my_weapon(c);
        
        dist[origin.i][origin.j] = 0;
        visited[origin.i][origin.j] = true;
        
        // First try to move just to the safest directions
        for (Dir d : Directions) {
            Pos new_p = origin+d;
            char temp_weapon = WEAPON;
            if (not pos_ok(new_p)) continue; // Went outside the board, don't do anything
            
            if (no_brainer(new_p.i, new_p.j, temp_weapon, is_warrior)) { // The cell is excellent, temp_weapon has been updated.
                // Going there is dangerous, wait patiently. (-board_enemy) = weapon strength of strongest adjacent enemy
                if (temp_weapon < -board_enemy(new_p)) return false;
                // There is no danger, go for it
                _move(VERY_HIGH_PRIORITY, ID, d);
                return true;
            }
            // A cell is safe if it's not adjacent to any dangerous cells (it will not become dangerous next round)
            bool safe = true;
            for (Dir e : Directions)
                if(pos_ok(new_p+e) and is_danger(new_p+e, WEAPON))
                    safe = false;

            // Add to Dijkstra queue the SAFE directions that are not: a wall, or dangerous, or attempting to attack a more powerful enemy.
            if (safe and board(new_p) != WALL and not is_danger(new_p, WEAPON) and -board(new_p) < WEAPON) {
                unsigned short int distance = 1; // By default, cost of movement in turns is 1
                add_movement_penalties(new_p, distance, WEAPON); // Add additional penalties to moving in that direction

                dist[new_p.i][new_p.j] = distance;
                Q.push({distance, new_p, d});
            }
        }
        // If no such direction exists, attempt to go to less safe directions
        if (Q.empty()) {
            for (Dir d : Directions) {
                Pos new_p = origin+d;
                if (not pos_ok(new_p)) continue; // Went outside the board, don't do anything
                
                // Add to Dijkstra queue the UNSAFE directions that are not: a wall, or dangerous, or attempting to attack a more powerful enemy.
                if (board(new_p) != WALL and not is_danger(new_p, WEAPON) and -board(new_p) < WEAPON) {
                    unsigned short int distance = 1; // By default, cost of movement in turns is 1
                    add_movement_penalties(new_p, distance, WEAPON); // Add additional penalties to moving in that direction

                    dist[new_p.i][new_p.j] = distance;
                    Q.push({distance, new_p, d});
                }
            }
        }

        // All initial directions have been enqueued, dequeue until priority queue is empty
        while (not Q.empty()) {
            Pos u = Q.top().pos;    // Dequeue position u
            Dir dir = Q.top().dir;
            Q.pop();
            if (visited[u.i][u.j]) continue; // If the edge has already been visited, don't do anything else
            visited[u.i][u.j] = true; // Mark edge as visited
            int distance = dist[u.i][u.j];
            
            //    warrior   and    u contains an enemy    and  my warrior is stronger  and    it will be at night when I get there 
            if ( is_warrior and board(u) <= ENEMY_BUILDER and is_stronger(c, u.i, u.j) and is_round_night(round() + dist[u.i][u.j]) ) {
                int profit = ATTACK_PROFIT - distance;
                // Don't check adjancent positions: assume enemies are able to survive until we get there

                // Increase profit of attacking warriors instead of builders
                if (board(u) < ENEMY_BUILDER) profit += WARRIOR_EXTRA_PROFIT;
                
                if (profit > best_profit) {
                    best_profit = profit;
                    best_dir = dir;
                    take_ownership = nullptr;
                }
                continue; // Can't walk over enemies: don't keep searching
            }
            else if (board(u) == MONEY) { // found money
                int profit = MONEY_PROFIT - distance;

                int closest_dist = (*bonus_distances)[u].closest_dist;
                // There's a closer citizen and it's approaching it (last round's distance was greater)
                if (profit > 0 and closest_dist < distance and closest_dist < (*bonus_distances_prev)[u].closest_dist) {
                    //profit /= 2; // Reduce profit
                    profit = 0;
                    // cerr << "citizen at" << origin << ": profit for money at " << u << " has been reduced" << endl;
                }
                if (profit > best_profit) {
                    best_profit = profit;
                    best_dir = dir;
                    take_ownership = nullptr;
                }
            }
            else if (NEED_HEAL and board(u) == FOOD) { // low on health and found food
                int profit = HEALTH_PROFIT - distance;
                if (life_lost_in_attack() >= c.life) profit += ABOUT_TO_DIE_BONUS;
                // Don't check adjancent positions: assume no one is interested in health

                if (profit > best_profit) {
                    best_profit = profit;
                    best_dir = dir;
                    take_ownership = nullptr;
                }
            }
            // found a weapon and I'm one of the closest citizens that are interested (no citizen beats me to it)
            else if (board(u) >= GUN and (*bonus_distances)[u].closest_dist >= distance) {
                if (is_warrior and board(u) > WEAPON) { // I need it: go for it
                    int profit = WEAPON_PROFIT - distance;
                    if (board(u) == BAZOOKA) profit += BAZOOKA_EXTRA_PROFIT*2;

                    if (profit > best_profit) {
                        best_profit = profit;
                        best_dir = dir;
                        take_ownership = nullptr;
                    }
                }
                else if ((*bonus_distances)[u].closest_is_friendly) { // I don't need it and closest citizen is friendly
                    // add 4 turn penalty in order to avoid stepping on it (don't steal it from myself)
                    dist[u.i][u.j] += 4;
                    // cerr << "citizen at" << origin << ": avoiding to step on " << u << endl;
                }
                else { // I don't need it and closest citizen is an enemy, try to steal it so the enemy doesn't get it
                    int profit = STEAL_WEAPON_PROFIT - distance;
                    if (board(u) == BAZOOKA) profit += BAZOOKA_EXTRA_PROFIT;
                    // cerr << "citizen at" << origin << ": could steal weapon at " << u << endl;
                    if (profit > best_profit) {
                        best_profit = profit;
                        best_dir = dir;
                        // I might try to steal this weapon: I'm the new closest citizen
                        take_ownership = &(*bonus_distances)[u];
                        take_ownership_dist = distance;
                    }
                }
            }
            
            // Keep searching on adjacent positions
            for (Dir d : Directions) {
                Pos new_p = u + d;
                // only add to queue if: inside the board, and not a wall, and...
                if (pos_ok(new_p) and board(new_p) != WALL) {
                    unsigned short int new_distance = dist[u.i][u.j] + 1; // By default, the cost in turns is 1
                    add_movement_penalties(new_p, new_distance, WEAPON); // Add additional cost in turns

                    // ...and distance can be improved
                    if (new_distance < dist[new_p.i][new_p.j]) {
                        dist[new_p.i][new_p.j] = new_distance; // Mark new pos as visited by updating its distance to origin
                        Q.push({new_distance, new_p, dir}); // Push new pos and propagate dir
                    }
                }
            }
        }

        // All vertices have been dequeued: go to best vertex or build barricade

        if (best_profit == INT_MIN) return false; // No good vertex found

        // Compute threshold for building barricades. Begin at -intinity: never build barricade
        int barricade_threshold = INT_MIN;
        // Meets conditions for being able to build (builder at day and not inside a barricade)
        if (is_day() and not is_warrior and board_barricades(origin) == 0) {
            // If I'm able to build more barricades, update threshold.
            if (num_barricades < max_num_barricades()) {
                /* if (is_round_night(round() + BUILDING_ROUNDS_NIGHT)) barricade_threshold = BARRICADE_THRESHOLD_NIGHT;
                else */ barricade_threshold = BARRICADE_THRESHOLD;
            }

            // If it's next to an improvable barricade, improve it unless there's an urgent interruption
            for (Dir d : Directions) {
                if (has_buildable_barricade(origin+d))
                    barricade_threshold = BARRICADE_INTERRUPT_THRESHOLD;
            }
        }
        // If best vertex wasn't worth it, build barricade instead
        if (best_profit <= barricade_threshold) return false;

        // If my movement will keep me in place and I'm in danger, abort
        int new_i = (origin+best_dir).i;
        int new_j = (origin+best_dir).j;
        if (is_danger(origin, WEAPON) and dist[new_i][new_j]>1) return false;

        // If this movement will also save me from an attack (I'm in danger), increase priority
        if (is_danger(origin, WEAPON)) {
            best_profit = RUN_PRIORITY;
            // If the next attack will kill me, increase the priority even more
            if (life_lost_in_attack() >= c.life) best_profit = RUN_DEATH_PRIORITY;
        }
        else if (board_enemy(origin+best_dir) == 0) { // Not in danger and the cell I want to go isn't adjacent to enemies
            // I'm not in a hurry: priority can be eliminated since no other player is able to interfere with my action
            best_profit = NOT_IMPORTANT;
        }

        if (take_ownership != nullptr) {
            take_ownership->closest_is_friendly = true;
            take_ownership->closest_dist = take_ownership_dist;
        }

        _move(best_profit, ID, best_dir); // Use profit as priority
        return true;
    }
    
    
    
    /* MAIN FUNCTIONS: The corresponding function gets called according to the citizen type and the board state (day */
    /* or night). First they call approach_target and then they treat exceptional situations, like having to escape. */    

    // id is the identifier of a builder and current round is day
    void builder_day_task(int id) {
        // A good position to move has been found, don't do anything else
        if (approach_target(id, false)) return;

        // Find a good place to build
        Pos pos = citizen(id).pos;
        // If there's an existing barricade, build there
        for (Dir d : Directions) {
            if (has_buildable_barricade(pos+d)) {
                _build(BUILD_PRIORITY, id, d); // Improve barricade and return
                return;
            }
        }
        // If I'm not able to build new barricades, don't do anything
        if (num_barricades >= max_num_barricades()) return;
        // Find empty place to build barricade
        for (Dir d : Directions) {
            // inside board and barricade can be built there (empty) AND IT'S GUARANTEED THAT NO ENEMY WILL BE THERE
            if (pos_ok(pos+d) and board(pos+d) == EMPTY and board_enemy(pos+d) == 0 and board_barricades(pos+d) == 0) {
                ++num_barricades;
                _build(BUILD_PRIORITY, id, d); // Build new barricade and return
                return;
            }
        }
        // If it fails, repeat without guaranteeing that enemies won't be there
        for (Dir d : Directions) {
            if (pos_ok(pos+d) and board(pos+d) == EMPTY and board_barricades(pos+d) == 0) {
                ++num_barricades;
                _build(BUILD_PRIORITY, id, d); // Build new barricade and return
                return;
            }
        }
    }
    
    // id is the identifier of a warrior and current round is day
    void warrior_day_task(int id) {
        // At day, crimes can't be commited: just search for bonuses
        approach_target(id, true);
    }

    

    inline bool is_escape_route(const Pos& pos, char my_weapon) {
        // A position is an escape route if the stongest enemy it's next to (-board_enemy(pos)) is weaker or equal than my weapon
        return pos_ok(pos) and board(pos) != WALL and -board_enemy(pos) <= my_weapon /* and board_barricades(pos) >= 0 */;
    }


    // id is the identifier of a builder and current round is night
    void builder_night_task(int id) {
        // At night, barricades can't be built. Look for bonuses and avoid being killed

        // A safe position to move has been found, don't do anything else
        if (approach_target(id, false)) return;

        // We have to wait, make sure current position is safe
        Citizen c = citizen(id);
        Pos pos = c.pos;

        if (-board_enemy(pos) > BUILDER) { // Not safe: next to an enemy that's stronger than a builder
            int priority = RUN_PRIORITY;
            if (life_lost_in_attack() >= c.life) priority = RUN_DEATH_PRIORITY; // If the next attack will kill me, increase the priority

            // If there's an existing barricade, go there
            for (Dir d : Directions) {
                if (pos_ok(pos+d) and board_barricades(pos+d) > 0) {
                    _move(priority, id, d); // Take cover in barricade
                    return;
                }
            }
            // No barricade, find best place to run
            int best_outcome = -1;
            Dir dir;
            for (Dir d : Directions) {
                if (is_escape_route(pos+d, BUILDER) and board(pos+d) > best_outcome) {
                    best_outcome = board(pos+d);
                    dir = d;
                }
            }
            if (best_outcome != -1) _move(priority, id, dir);
            // Else no escape route was found: accept my fate and don't do anything
        }
    }

    // id is the identifier of a warrior and current round is night
    void warrior_night_task(int id) {
        // At night crimes can be commited: search and attack closest safe target

        // A safe position to move has been found, don't do anything else
        if (approach_target(id, true)) return;

        // We have to wait, make sure current position is safe
        Citizen c = citizen(id);
        Pos pos = c.pos;
        char WEAPON = my_weapon(c);

        if (-board_enemy(pos) > WEAPON) { // Not safe: next to an enemy that's stronger than my weapon
            int priority = RUN_PRIORITY;
            if (life_lost_in_attack() >= c.life) priority = RUN_DEATH_PRIORITY; // If the next attack will kill me, increase the priority
            
            // If there's an existing barricade, go there
            for (Dir d : Directions) {
                if (pos_ok(pos+d) and board_barricades(pos+d) > 0) {
                    _move(priority, id, d); // Take cover in barricade
                    return;
                }
            }
            // No barricade, find best place to run
            int best_outcome = INT_MIN;
            Dir dir;
            for (Dir d : Directions) {
                if (is_escape_route(pos+d, WEAPON) and board(pos+d) > best_outcome) {
                    best_outcome = board(pos+d);
                    dir = d;
                }
            }
            if (best_outcome != INT_MIN) _move(priority, id, dir);
            // Else no escape route was found: accept my fate and don't do anything
        }
    }
    
    
    // Play method, invoked once per each round.
    virtual void play () {
        // Initialize data structures
        if (sz_n == 0) initialize();

        if (round() & 1) { // Odd round
            bonus_distances = &bonus_distances_odd;
            bonus_distances_prev = &bonus_distances_even;
        }
        else { // Even round
            bonus_distances = &bonus_distances_even;
            bonus_distances_prev = &bonus_distances_odd;
        }
        build_board();
        num_barricades = barricades(me()).size();
        
        // Get info on current units.
        vector<int> BLD = builders(me()); 
        vector<int> WAR = warriors(me());
        
        if (is_day()) {
            for (int id : BLD) builder_day_task(id); // Iterate on all builders
            for (int id : WAR) warrior_day_task(id); // Iterate on all warriors
        } else {
            for (int id : BLD) builder_night_task(id); // Iterate on all builders
            for (int id : WAR) warrior_night_task(id); // Iterate on all warriors
        }
        
        // Now send all the commands stored in the buffer
        while (not instruction_buffer.empty()) {
            Instr com = instruction_buffer.top();
            if (com.is_build) build(com.id, com.dir);
            else move(com.id, com.dir);
            instruction_buffer.pop();
        }
    }
};

/* =================================== */
/* In memory of PapaTormenta, who came */
/* back from the dead to save us all.  */
/*    May you always be remembered.    */
/* =================================== */


// Do not modify the following line.
RegisterPlayer(PLAYER_NAME);
